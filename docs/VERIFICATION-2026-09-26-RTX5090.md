# RTX 5090 on 0.26.1: live pace re-measured, and five defects the run found

Verified 26 September 2026 on the machine of the
[0.17.0 RTX 5090 record](VERIFICATION-2026-09-10-RTX5090.md), because the speed
the README and the website quoted for this GPU - 8.4, 15.4 and 42.0 ms/frame at
1080p, 1440p and 4K - came from 0.17.0 on the previous runtime, and nothing
since had measured it.

The quoted paces were stale. On 0.26.1 with the fixes below, the same three
geometries cost **6.75, 9.90 and 22.57 ms/frame**, and all three now keep up
with 30 fps playback, 4K included. Getting there found five defects, all fixed
here and released in 0.26.2.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Microsoft Windows 11 Pro` / `10.0.26200` | `Win32_OperatingSystem` |
| CPU / RAM | AMD Ryzen 7 9800X3D, 30.9 GiB | `Win32_Processor`, `Win32_ComputerSystem` |
| GPU | `NVIDIA GeForce RTX 5090`, driver `616.64`, `0x2B8510DE` | `nvidia-smi` |
| Repo | `5726956` (0.26.1) plus the fixes below | `git rev-parse HEAD` |
| Build | Release, Visual Studio 2022 Build Tools, clean rebuild in 82 s, zero warnings | build log |
| Runtime | ReShade 6.8.0.2155, RenoDX 6.5.3, DLSS SR 310.9.1, DLSS-NR `310.8.SF-v2` | render receipt, `stage_runtime.ps1` |
| FFmpeg | 9.0.1 (staged) | `ffmpeg -version` |

## Gates

Before any change, on the checkout as it was:

- Clean rebuild: 0 warnings, 0 errors.
- `ctest -LE "gpu|audio"`: **14/14 passed**.
- `ctest -L "gpu|audio"`: **16/18**. `FrameGenerationSmoke` failed 3 of 3
  (`worstPassthroughDeltaPx=1.914` against a 1.0 px bound) and
  `AudioClockSmoke` 4 of 5 (`seek right after start: worst 252-317 ms`
  against 250). Both are explained and fixed below.

After the fixes: 0 warnings; portable **14/14**; hardware **18/18**, with
`AudioClockSmoke` passing six runs in six (seek right after start 47-95 ms).

## The runtime on this machine was stale

`external/runtime` and the build's `neural-runtime` still held DLSS SR 310.8.0
and RenoDX 4.70, while `packaging/runtime-lock.json` pins 310.9.1 and 6.5.3. The
player refused every live session, correctly and by name:

```
Neural runtime lock drift; render refused: nvngx_dlss.dll: size 58956400, hash mismatch; renodx-dlss5.addon64: size 1732608, hash mismatch
```

`tools/fetch_neural_runtime.ps1` fetched the two archives (30.3 MB and 0.37 MB)
and `tools/stage_runtime.ps1` staged all 12 locked files. A rebuild does not
restage the runtime, so a checkout that fetched before the lock moved keeps the
old files until `stage_runtime.ps1` is run again;
[BUILDING](BUILDING.md#add-the-experimental-runtime) now says so. The GPU
smokes passed on the stale runtime, since they drive the worker without the
player's lock check: a green hardware suite does not say the player will
render.

## Measured pace

The same method as the 0.17.0 record, now driven by
`tools/verification/player_session.ps1`: open the clip, toggle at about 1 s,
let the render finish, toggle off, read `Measured neural render pace`. The
render cache was dropped before every session, so every session rendered its
whole range (about 700 frames after the first segment). Medians:

| Geometry | 0.17.0 ms/frame | 0.26.1 ms/frame | Sessions | Spread | Real time | Dropped |
| --- | --- | --- | --- | --- | --- | --- |
| 1920x1080 30 | `8.355` | **`6.75`** | 5 | 6.70 - 6.83 | 4.94x | 0 of ~210 |
| 2560x1440 30 | `15.444` | **`9.90`** | 3 | 9.86 - 9.91 | 3.37x | 0 of ~270 |
| 3840x2160 30 | `42.03` | **`22.57`** | 3 | 22.22 - 22.58 | 1.48x | 0 of ~480 |

```
Measured neural render pace: 1920x1080 at 100% processing scale, 6.75405 ms/frame over 698 frames (0.538858x the reference GPU); ...
Measured neural render pace: 2560x1440 at 100% processing scale, 9.89651 ms/frame over 697 frames (0.597399x the reference GPU); ...
Measured neural render pace: 3840x2160 at 100% processing scale, 22.5694 ms/frame over 700 frames (0.80358x the reference GPU); ...
```

Toggle to first neural frame: 7.88 - 8.24 s warm, 10.84 s for the first
session after a fresh profile (preflight included).

The same bytes tagged BT.709 limited with `h264_metadata` (no re-encode), three
sessions each: **6.72, 9.83 and 23.22 ms/frame**, nothing dropped. A tagged
source never took the path defect 3 below fixes, so these are what 0.26.1
already does on the usual file, a YouTube stream included; the untagged figures
above need the fix.

Sources: the repo's 1080p demo (`docs/media/neural-comparison-demo.mp4`, now
24.8 s, 744 frames, 2.0 Mbit/s H.264) and two re-encodes of it made with the
0.16.0 record's recipe (`scale=...:flags=lanczos -r 30 -c:v libx264 -preset
veryfast -crf 18`): 2560x1440 at 3.2 Mbit/s and 3840x2160 at 6.0 Mbit/s. All
three declare no colour matrix.

## Defects found and fixed

### 1. The measuring script no longer read the player's log

`player_session.ps1` waited for `Active neural session started at X s through
Y s`; the player has logged `Active neural session starting at X s over
[A,B) s.` since the whole-video render. Hearing nothing, the script pressed
the toggle again - which turned the session it had just started off, 170 ms
after it attached. Its pace pattern also predated the processing-scale field
(`1920x1080 at 100% processing scale, 6.75 ms/frame`) and its cache-entry
pattern the `wholeRange=` field. All three patterns now match, checked against
a real session log.

### 2. A session opened mid-video never recorded its pace

With the script fixed, five 1080p sessions of about 700 frames logged no pace
at all. A session opened at 1 s renders to the end, then starts a second job
for the head it skipped. `NeuralSegmentIndex::ResetPace` restarts the pace
clock for that job, so what remained at stop was the head job's own pace: one
30-frame segment, below the 120-frame floor, so nothing was recorded and the
forecast never learned this GPU from a session that ran to the end. The index
now keeps the longest finished job's pace across a reset and the stop records
`MeasuredPace()`, the larger of that and the current job's
(`neural_segment_index_keeps_the_longest_job_pace_across_a_reset_test`).

### 3. Untagged HD video decoded through the CPU, and 4K could not play

With pace recorded, 4K sessions measured 30.9 ms/frame - faster than real
time - yet presented 233-307 frames and dropped 350-421, falling to 3.5 fps.
The log said why:

```
GPU source conversion refused: matrix=unspecified range=unspecified ...; no conversion implements that description, so the source decodes to BGRA and ffmpeg converts it on the CPU instead.
```

Since 0.26.0 an untagged HD video is read as BT.709 limited
(`UntaggedColorPolicy.h`), and the CPU path pins exactly that, but the NV12
gate still asked what the file declared and refused. Every untagged HD file,
the original and each of its neural segments, went through a 33 MB-per-frame
BGRA pipe at 4K. The same bytes tagged BT.709 with `h264_metadata` played
30 fps with nothing dropped and rendered at 22.9 ms/frame.

The decoder now gates NV12 on the description it decodes under
(`VideoDecoder::DecodedColor`, `DecodedColorDescription`), and the renderer and
the worker's evaluator are handed the same one, so either layout reads an
untagged HD video as BT.709 limited. Untagged SD has no such rule and still
decodes as BGRA. A render made with GPU source conversion on (Encoder settings,
off by default) now converts such a source on the GPU where it used to be
refused, so its cache key carries `untagged-hd-bt709-gpu-v1`. With the setting
off nothing about a render changes and neither does its key. Tests:
`untagged_hd_video_is_decoded_under_a_description_the_gpu_converts_test`, and
an NV12 case in `UntaggedVideoDecodesWithTheMatrixItsSizeImpliesTest` that
fails on the old gate and checks that the stored Y'CbCr samples pass through
untouched.

Before and after on this build (medians):

| Geometry | Refused (BGRA) | Converted on the GPU |
| --- | --- | --- |
| 1920x1080 30 | 6.87 ms, 0 dropped | 6.75 ms, 0 dropped |
| 2560x1440 30 | 11.20 ms, 0 dropped | 9.90 ms, 0 dropped |
| 3840x2160 30 | 30.88 ms, 350-421 dropped | 22.57 ms, 0 dropped |

### 4. FrameGenerationSmoke measured its source under another reading

`external/test-media/dlaa-smoke.mp4` is present here and not on the RTX 4080
machine, which runs on the generated BT.709-tagged stand-in. It is untagged
1280x720 whose samples are BT.601, so the player reads it as BT.709; its
saturated bars leave the RGB cube under that reading and clip. Every
passthrough frame came back 1.914 px off the source's stored luma. The pass was
not at fault: tagging the same bytes BT.601 gave 0.131 px, and the source put
through ffmpeg's own BT.709 round trip averaged Y 125.09 against the output's
125.09 (stored source: 125.97). The harness now asks the player's decoder
whether the untagged rule applies and, when it does, measures the source
through that round trip. Worst passthrough: 0.682 px on `dlaa-smoke.mp4`,
0.003 px on the textured probe (was 0.194).

### 5. A seek right after a start reopened the audio endpoint cold

Instrumented, every step of `WasapiRenderer::Open` took under 2 ms except
`IAudioClient::Initialize`: **211-232 ms** when the endpoint's previous stream
had just been released, 7-12 ms otherwise. A seek released the stream, then
initialized a new one. `AudioPlayer::Start` now holds the stream it replaces,
stopped, until its own is open; an exclusive (passthrough) stream is released
first as before, since it owns the endpoint. Over six runs: seek right after
start 252-317 ms -> 47-95 ms; format-change restart 0.32 s -> 0.08-0.10 s.
Stop right after start went from 35-40 ms to 35-82 ms in some runs, inside the
150 ms bound the smoke asserts. The follow-up below found why, and it is not a
wait running out.

## Follow-up: the items this run left open

Worked the same day on the same machine, after 0.26.2 shipped.

**Packaged player dropped 3-6 frames per session; the build tree dropped none.**
Every packaged session dropped them at one instant: when its render finished.
Timed inside, the completion handler spent 88-102 ms in
`SweepRetiredLiveSegments`, deleting the eleven segment files the joined cache
entry had just replaced, on the thread that presents, and playback fell 103-113
ms behind. The packaged player's cache is under `%LOCALAPPDATA%` (an older
install's folder exists there); the build tree's is beside the executable on
another drive. Deleting the same number of fresh files from a script took 5 ms
on either drive, so the cost is in what the system does to files a helper has
just written. `BackgroundFileReaper` now deletes them on its own thread; the UI
thread still decides what is safe to delete. After: the handler takes about
5 ms, and ten packaged sessions in a row showed no stall at completion. What
remains is 0-2 frames at the instant the neural picture first appears (counted
before the first health window after attach), none during neural playback.

**Stop right after start at 82 ms.** Not the waits running out, as the section
above first said. With every phase of `AudioPlayer::Stop` timed, ending the
child and joining the reader took 3 ms each time; the fade took 25-32 ms when
nothing had been queued yet (`tail not needed (already silent)`: the 24 ms
grace and little else) and 78-80 ms when audio was already playing (`tail
queued`: room for the tail, about 31 ms for one 1056-frame buffer to play out,
then the grace). 80 ms is the designed cost of a click-free stop while sound
plays, the same as a stop mid-playback (63-80 ms in the same log); a start that
no longer waits 220 ms on the endpoint gets sound flowing before more of these
early stops arrive. No change.

**`NeuralPrerenderTests`, once in sixteen runs.** The staging sweep has a
100 ms budget and leaves the rest to the next sweep, by design. Five removals
take about 9 ms (median of 63 runs); the failing run's sweep took 150 ms and
logged `removed=4 remaining=1`. The test now lets later sweeps finish, as the
next start would. 15 consecutive passes.

**`NeuralWorkerTests` crash dump, once on CI** (run 36230306450). The handler
wrote its line but no dump, and never printed why; 88 local runs, 48 of them
eight at a time, did not repeat it. The handler now retries once with a minimal
dump and logs the full dump's winerr either way, and the test prints the
handler's line whenever there is no dump, so the next failure names its cause.

## Still open

- The RTX 4080 SUPER's live pace, 15.31 ms/frame at 1080p, is from 0.16.0 and
  was not re-measured; the README says so.
- The 1440p and 4K clips are re-encodes of the 1080p demo, not native captures.
  A heavier 4K file decodes more slowly; the player measures its own sessions
  and warns before one it expects to fall behind on.
- The seed constants in `src/PlaybackTiming.h` stay at the 0.16.0 measurement.
  They now overstate this GPU about 1.85x at 1080p, which asks before a marginal
  first session rather than dropping frames in it; one session replaces them.
- Only the live-session path was timed. Export, frame generation and YouTube
  input were covered by the hardware suite, not re-measured.
