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
- Two defects seen in these sessions were left untouched by the work above and
  are fixed in the second round below: a completed render whose publish failed
  with `The neural video failed final cache validation`, and a re-toggle after
  full coverage that starts a zero-length session and reports `The requested
  render range lies outside the source`.
- Only the live-session path on a YouTube source was exercised. Export, local
  files, packaging and visual quality were not.

## Second round, same day: the two remaining defects, and two additions

Same machine, same driver, same pin. The two defects listed above as not
diagnosed turned out to be one chain, and the session that proved them fixed
also carries an evidence floor the render did not have and a presentation
control the player did not have.

### The publish gate refused a completed render

A session that rendered and verified every frame lost its cache entry:

```
Neural render receipt: ... frames=2607/2607 verified=2607 failure=none lock=ok
Active neural session ended early: detail=The neural video failed final cache validation
```

The gate is three pairwise duration comparisons plus an exact frame count. The
first hypothesis was accumulating muxer rounding: a live session's entry is not
one file, `ConcatenateMedia` joins the segment files the render published, each
muxed on its own onto Matroska's 1 ms grid, and a 47-part session passed where a
44-part one failed. The tolerance was sized to the join on that basis
(`RuntimePolicy::JoinedMediaDurationTolerance100ns(fps, parts)`: one frame of
playback jitter plus one 1 ms rounding per joined file, so 1 part is 343334 ticks
and 44 parts 773334), and the refusal was given a diagnostic, because it had left
nothing to diagnose it with.

**That hypothesis was wrong, and the diagnostic is what proved it.** The next
session to fail printed this:

```
Neural publish refused: renderOk=1 manifestReusable=1 probeOk=1
  probeFrames=2622  resultFrames=1647
  probeDuration=874000000  resultDuration=548999999  expectedDuration=548996687
  tolerance=793334  parts=46
```

87.4 s and 2622 frames in the file against 54.9 s and 1647 frames in the result.
No accumulation of 1 ms roundings produces that. The join took **every** segment
in the session's index, while the manifest, the cache key and the evidence
counters describe only the job that just finished. This session had attached,
been cancelled and re-attached twice (`failure=cancelled frames=0/0` twice
before the 1647-frame render), so the index held earlier coverage, and the entry
would have claimed a range and a proof it did not have. The gate was right to
refuse it.

Fixed where the mislabel is: the join now takes exactly the segments this job
published, from the index position the session handed it. The file then matches
its own label, and earlier coverage stays in the index for playback to keep
reading. The frame-count equality is deliberately left exact: a join that loses
or gains a frame is a real defect and must stay fatal. The tolerance work above
is kept, because per-file rounding is real, just not what refused these renders.

Live proof, this tree, two sessions:

```
Neural render receipt: ... frames=2656/2656 verified=2656 failure=none lock=ok
Active neural session rendered 2656 frames and published its cache entry; save=1

Neural render receipt: ... frames=2082/2082 verified=2082 failure=none lock=ok
Active neural session rendered 2082 frames and published its cache entry; save=1
```

### The re-toggle that started a zero-length session

```
Active neural session started at 94.7333 s through 94.7333 s; resumed on 47 retained segments
Active neural session ended early: kind=source detail=The requested render range lies outside the source
```

`StartLiveNeuralSession` compared a head built from integer per-frame segment
ends against a range end taken from the probed duration, so a residual below one
frame passed the `renderFrom >= range.end100ns` guard and spawned a worker that
immediately refused the range. `RuntimePolicy::RenderRangeIsCovered` now treats a
remainder shorter than one frame as coverage and takes the existing replay path,
and the same predicate drives the `Unfinish()` guard so a covered range is not
un-finished and re-finished.

Note how it was reached: the retained segments came from a session whose publish
had just failed, which is the defect above. Fixing the gate removes the usual way
in, which is why this one is pinned by a test rather than by a session.

### An evidence floor on median neural GPU time

The evidence chain accepted a run in which feature 18 was created and evaluated
but the neural pass did not execute: the roadmap's open gap records
`frames=900/900 verified=900` for output that was DLAA only, at 0.46 ms of neural
GPU time per frame against a healthy 5.7 ms. Every counter the chain checks was
satisfied. What a DLAA-only run cannot fake is GPU cost, and the render loop
already measured it: `NeuralRenderTiming::neuralGpuMsP50` has travelled the
worker protocol into `receipt.json` since 0.14.1 with nothing consuming it as a
verdict.

`NeuralTimingClearsFloor` now refuses a run whose median falls below
0.59 ms per output megapixel, 1.223 ms at 1920x1080. The constant is the
geometric midpoint of the two ends in evidence, which are only 7.1x apart:

| Point | Median at 1920x1080 | Source |
| --- | --- | --- |
| DLAA-only failure | `0.46 ms` | roadmap item 1 open gap |
| **floor** | **`1.223 ms`** | 2.66x above the failure |
| Benchmark reference run | `3.26 ms` | `docs/BENCHMARK.md` reference table |
| Three render receipts on this machine | `3.683776`, `3.696608`, `3.715776 ms` | `cache/v1` receipts, 2534 to 2658 samples |

The remaining risk is one-sided in the safe direction: per-pixel neural cost only
rises on slower hardware, so a fixed floor can misjudge a healthy run only on a
GPU substantially faster than a 5090, while the failure it catches sits 7x below
it on the fastest card that exists today. A run with no timing samples is still
accepted, because a build without timing instrumentation reports those fields as
zero and a missing measurement is not a verdict. `MediaGpuSmoke` renders at
640x360 and 1920x1080 cleared the floor unchanged (`overall=PASS`).

### A strength dial that costs a present, not a render

Every knob that changes how much of the neural result you see is a model
parameter written into `ReShade.ini`, and the canonicalised `[RenoDX.DLSS5]`
block is hashed into the render identity, so each distinct value is rendered from
scratch: 16 cold single-frame previews at a median of 10.6 s each against 1.03 s
for an already-rendered combination (`docs/BENCHMARK.md`).

The add-on overwrites the NGX output UAV in place, so there is no separable
neural answer to re-compose. What the presentation shader does hold is the
composed neural frame at `t0` and the original at `t1`, already bound on every
present draw for the comparison modes. Re-mixing those two is a new
presentation control, `[VideoAdjustments] NeuralStrength`, 0 to 200 percent in
the image adjustments window, carried in two root constants that were literal
zeros. Below 100 percent it mixes back toward the original; above it, it extends
the luminance ratio the model produced, as a ratio and never an additive delta,
with a two-sided guard of 2.0, a 1/512 floor on both terms, one scalar applied to
the whole RGB triple, and peak normalisation on encode. The rules are RenoDX's
(MIT, read directly rather than through a GPLv3 fork).

At 100 percent the composite branch is not entered and the reference upload
returns early, so the default path is the pre-change path. The capture and export
paths pass `useReference=false`, which forces the strength to 1, so every cached
render, every export and every digest is untouched, and the cache is not
orphaned.

Measured on one paused frame of cached playback, driving the trackbar in a single
run and reading back the composited render area (1365x768):

| Dial | Mean luma | Pixels changed against 100 % | Mean channel delta |
| --- | --- | --- | --- |
| 0 % | `130.827` | `52.91 %` | `4.830` |
| 100 % | `126.193` | - | - |
| 100 % again | `126.193` | `0.00 %` | `0.000` |
| 200 % | `122.319` | `51.74 %` | `3.924` |

Of the 52.0 % of pixels where the neural frame differs from the original by more
than one luma step, the 200 percent extension moves 99.8 % of them in the
model's own direction, by a mean of 7.78 luma steps against the model's own 9.43.
That is the composite following the model rather than pushing contrast. Two
captures at the same dial position are bit-identical, so those differences are
the control and not capture noise.

### Third round: the source the player was actually playing

A user report that a trailer "feels very low bitrate" turned out to be two
separate defects, both measurable.

**Auto asked for the bottom rung.** `YouTubeSourceQuality::Auto` selected exactly
1080p, and on YouTube that is the lowest-bitrate rung of a trailer. Measured with
the bundled yt-dlp 2026.08.19 through this player's own selector arguments:

| rung | `Tg1oRHd5zlw` | `VQRLujxTm3c` |
| --- | --- | --- |
| 1080p | `3899 kbps` (avc1) | `4604 kbps` (avc1) |
| 1440p | `7854 kbps` (vp9) | `9282 kbps` (vp9) |
| 2160p | `20764 kbps` (vp9) | `18971 kbps` (vp9) |

Auto now takes the tallest rung up to 1440p and the highest advertised bitrate
inside it. The cap is deliberate: 1440p is about twice the bitrate for about 1.8x
the render cost, where 4K is 42 ms/frame, 0.78x real time, and four times the
VRAM and cache footprint. Verified live, same trailer:
`ffprobe: 2560x1440 DAR=1.77778 @ 59.9401 fps` and
`YouTube source selected: 1440p at 7854.43 kbps, age limit 18`, against
`640x360 @ 29.97 fps` before.

**A 360p fallback reached the screen with nothing said about it.** The reported
session played `640x360` at `451 kbps` and ran its neural render at that
resolution. The cause is not ours: the video is age-restricted, so an anonymous
session is offered one legacy progressive format. `yt-dlp -F` on it prints
`This video is age-restricted; some formats may be missing without
authentication` and lists only `18  mp4  640x360  451k`. Which path you land on
is not stable: three consecutive Auto resolves of the same URL returned 1440p,
1440p, then 360p. Three of the six bundled example trailers are age-restricted.

The resolver now reports the height, the bitrate and the age gate, and the player
states them. Caught live on a run that happened to land on the fallback:

```
YouTube source selected: 360p at 0 kbps, age limit 18.
YouTube served a degraded source: Source is only 360p at 0.0 Mbps: this video is
age-restricted, and YouTube serves higher quality only to a signed-in session
```

That `0.0 Mbps` is why the print template now falls back to `tbr`: a progressive
format advertises no separate video rate. Paths that reach the screen without a
resolve - a cached copy an earlier session acquired at a degraded height, a seek
that reuses its URLs - report the height alone, from the decoder.

**One more defect, found on the way.** A recent-history entry outlives the cache
folder. With the entry naming a source copy that had been deleted, a job was
handed the key, found nothing, and a live session ended on
`kind=none covered=0 detail=` - an empty sentence and no picture. The key is now
verified against the cache before it is used, a job that still finds the copy
gone re-acquires the stream it is already playing, and the unrecoverable case
carries a sentence. After the fix the same sequence renders and publishes:
`frames=2082/2082 verified=2082`, `published its cache entry`.

### Gates

`ctest` 13/13, zero compiler warnings, `MediaGpuSmoke` `overall=PASS`.

Every new test was checked by mutation rather than by assumption: dropping the
per-part term and the sub-frame residual from the two predicates fails 5
assertions in `PolicyTests`, zeroing the floor constant fails 4 in
`NeuralPrerenderTests`, and moving the dial's default off 1.0 fails 2 in
`PlayerUiRegressionTests`.

### Limits of this round

- The `dropped=5` of 2638 presented frames in the dial session is not a clean
  number: that run was driven by a script that repeatedly stole the foreground
  and read the composited desktop. It is reported, not claimed.
- The publish-gate diagnostic fired on the very next failure and is what
  disproved the rounding hypothesis. Its output is quoted above.
- The dial is presentation-only by choice. Baking a composited look into an
  export would cost the MKV branch its stream copy, and the receipt would have to
  record the composition parameters for the export to be reproducible.
- The dial needs a resident original/neural pair, which exists during cached
  playback with the neural view. It degrades to 100 percent everywhere else.
- The bitrate ladder is one machine, one day, two trailers. YouTube's per-video
  encodes differ, and an age-restricted video can answer differently on the next
  call, as it did here three times out of three.
- Auto's 1440p cap is a judgement, not a measurement of this GPU: the 1.8x render
  cost is scaled from this README's own per-geometry figures, not timed at 1440p
  on this card. A slower GPU will need the existing pace confirmation to refuse
  the session, which is the path that already exists for it.
- Nothing here authenticates to YouTube. Cookies would unlock the full ladder for
  age-restricted videos and were not implemented or tested.
- The neural render still runs at whatever the source turned out to be. The
  player now says the source is 360p; it does not refuse to render one.
