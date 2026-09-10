# RTX 5090 on 0.17.0: live-session pace, and a seam that stopped playback

Verified 10 September 2026 on the same machine as the
[0.16.0 RTX 5090 record](VERIFICATION-2026-09-09-RTX5090.md), to check the speed
claims the README makes for this GPU after 0.17.0 pipelined the export loop.

Two results: the quoted paces were stale at 1080p and 1440p, and repeating the
1080p session exposed a defect that ends live playback with the modal "out of
sync" warning at the first segment boundary.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Microsoft Windows 11 Pro` / `10.0.26200` | `Win32_OperatingSystem` |
| GPU | `NVIDIA GeForce RTX 5090`, driver `616.64`, `0x2B8510DE` | `nvidia-smi --query-gpu=name,driver_version,pci.device_id` |
| GPU (DXGI) | DXGI driver `32.0.16.1664` | render receipt |
| Repo commit | `e297ceb` plus the fix below | `git rev-parse HEAD` |
| Build | Release, `cmake --build --parallel`, zero warnings | build log |
| Runtime | ReShade 6.8.0.2155, RenoDX 4.7, DLSS-NR 310.8.0, `310.8.SF-v2` | render receipt |

Sources: the repo's 30 s 1080p30 demo (`docs/media/neural-comparison-demo.mp4`)
and the two 30 s re-encodes of it the 0.16.0 record synthesized, so every number
below compares against that record clip for clip.

Sessions were driven the same way as in that record: launch the player on the
file, post `D` three seconds after the window appears, hold, post `D` again, and
read `Measured neural render pace` from the log. The cache directory was removed
before each session, so every one rendered.

## Gates

- `ctest --test-dir build-upscaling -C Release`: **12/12 passed**, 43.10 s.
- `MediaGpuSmoke.exe external/ffmpeg/bin …/NeuralWorker.exe smoke-2`: exit 0,
  `overall=PASS`, `neural_ok=1` on photo, GIF and video with
  `verified_frames` equal to `frames` in each.

## Measured pace

Medians of the sessions whose playback ran to the end of the clip; 0.16.0 is the
single session per geometry in the previous record.

| Geometry | 0.16.0 ms/frame | 0.17.0 ms/frame | Sessions | Spread | 0.17.0 / 0.16.0 | Real time |
| --- | --- | --- | --- | --- | --- | --- |
| 1920x1080 30 | `11.888` | **`8.355`** | 8 | 8.25 – 9.05 | 0.70x | 3.99x |
| 2560x1440 30 | `17.149` | **`15.444`** | 3 | 15.33 – 15.80 | 0.90x | 2.16x |
| 3840x2160 30 | `42.870` | **`42.03`** | 2 | 41.54 – 42.52 | 0.98x | 0.79x |

```
[01:52:29.220] Measured neural render pace: 1920x1080 at 8.25198 ms/frame over 817 frames (0.658367x the reference GPU); 3 geometries known for this GPU.
[01:56:16.117] Measured neural render pace: 2560x1440 at 15.4436 ms/frame over 813 frames (0.932248x the reference GPU); 3 geometries known for this GPU.
[01:57:07.082] Measured neural render pace: 3840x2160 at 41.54 ms/frame over 660 frames (1.47903x the reference GPU); 3 geometries known for this GPU.
```

1080p came down 29% and 1440p 10%, while 4K did not move. The 4K clip is a
6315 kbit/s re-encode: its decode and its NVENC encode, not the neural pass, set
the pace, which is what 0.17.0 did not touch. At 1440p the worker's own stage
report puts guides at 1.19 ms of a 13.60 ms loop, against 4.6 ms of guides
before the change.

A session whose playback dies early measures the render without the player's
decode and present beside it, and reads about 20% faster: the two sessions the
defect below killed reported `7.968` at 1080p and `12.438` at 1440p. Those are
excluded from the table.

## Keep-up forecast

With only its fresh 1080p sample the player warns before 4K30, which the 0.16.0
record could not confirm (its 4K session predated the per-geometry fix and
started with no prompt, dropping 848 of 869 frames):

```
PROMPT title='Neural rendering from here' body=' | &Yes | &No | This video is 3840x2160 at 30 fps. On this GPU neural rendering runs at about 23.5 frames per second, which is 0.78x real time, so watching it live would pause to buffer almost continuously.
```

Answering Yes ran the session at `41.54` ms/frame with `presented=14
dropped=583`, so the prompt's 0.78x matched the outcome.

## Defect: live playback stops at the first segment seam

Two of five 1080p30 sessions stopped about two seconds after playback attached,
at the last frame of segment 0, and raised the modal
`Neural playback fell out of sync` warning. The render was unaffected: it went on
to finish every frame and publish its cache entry.

Both failures were sessions whose playback attached one frame *inside* the first
segment rather than at its start.

Cause. `DecodeSegment` rebuilds a segment's exclusive end as
`firstTimestamp100ns + frameCount × frameDuration100ns`, with the frame duration
an integer number of ticks. At 30 fps that is `333333` against a true
`333333.33`, so segment 0 declares an end 20 ticks below segment 1's own first
pts, and the gap grows by 20 ticks per seam. `NeuralSegmentIndex::Containing`
found no segment for a playhead inside that hole, and `ClassifyUncovered` reads
an uncovered timestamp between the render start and the render head as a producer
contract break, which is `OutOfSync`. Whether the playhead landed in the hole
depended on the seek: a mid-segment attach seeks the original decoder, and a
seeked FFmpeg source stamps its timestamps a few ticks below the CFR grid.

The diagnostic added in this run names it directly:

```
[01:46:07.022] Neural playback out of sync: segment-hole original=86@28666666 neural=86@28666666 numbered=1 segment=0 exhausted=0 head=188999980 segments=9 finished=0; position=2.86667 presented=57 dropped=2
```

`original=`/`neural=` are the last pair that matched, frame 86 at 2.8667 s; the
read that failed is the next frame, 87, whose 2.9 s landed in the hole.
`head=188999980` for nine 60-frame segments starting at 0.9 s is the same
rounding: 20 ticks short of `189000000` per seam, times nine.

Fix. `NeuralSegmentIndex::Append` closes a hole narrower than one frame by
extending the previous segment's end to the next segment's first pts, and
`SelectSegment` looks a segment up by frame number when the playhead frame
carries one, which is what the coverage test beside it already did. A gap of a
frame or more - a relaunch rebased further ahead - stays uncovered.

Regression tests in `tests/NeuralPrerenderTests.cpp`
(`live_playback_crosses_a_seam_whose_end_rounds_below_the_next_start_test`,
`neural_segment_index_covers_the_rounding_hole_but_not_a_real_gap_test`) build
segment records the way the protocol does and fail on the old code at the seam.

After the fix, four consecutive 1080p30 sessions played to the end
(`presented=871/866/863/864`, `dropped=5/6/8/9`, no warning), two of them
attaching mid-segment, and the 1440p and 4K sessions behaved as tabulated above.

## Limits

- One machine, one 30 s clip per geometry, and the 1440p and 4K clips are
  re-encodes of the 1080p demo rather than native captures. Their absolute cost
  includes that decode.
- The RTX 4080 SUPER was not available here. Its `15.31` ms/frame at 1080p and
  the `1.22` Ada prior derived from it both predate the pipelined loop, so that
  prior now overstates Ada's cost by an unmeasured amount.
- The reference constants in `src/PlaybackTiming.h` were left at the 0.16.0
  measurement. Refitting them on this run's three points puts the fixed term
  below zero, because the 4K point is dominated by its source rather than the
  GPU, and a seed that overstates cost asks before a marginal first session
  instead of dropping frames in it.
- Presentation still drops 5 to 9 of about 870 frames per 1080p session against
  0.16.0's 5. Not investigated here.
- Only the live-session path was exercised at these geometries. Export, YouTube
  input, packaging and visual quality were not.
