# The driven player session: toggle-to-picture measured, and the Ada pace bracketed

Verified 14 September 2026 on an RTX 4080 SUPER. Every player-side number this
project has published so far has been an estimate, for one structural reason:
the benchmark harness drives `NeuralWorker.exe` headlessly and never opens the
player, so the toggle, the attach, the receipt written to disk and the
`Neural cold start:` line have only ever had unit coverage. This record is the
first from the other side of that gap. `tools/verification/player_session.ps1`
launches the real player on a real clip, injects the player's own neural-render
accelerator through `SendInput`, and reads `DLSSVideoPlayer.log` until the
render reports back.

Four results, from ten passing sessions on an idle GPU. **Toggle-to-first-neural
-frame is 8.39-9.18 s on the first toggle after an install and 4.88-5.16 s on
every later one**; the 3.8 s difference is the feature-18 preflight probe, which
is a second helper process whose verdict is cached per runtime identity, not per
toggle. The player's own request-to-picture stopwatch agrees with the wall clock
to within 8-19 ms in ten of ten, so the instrument and the instrumented agree.
**`receipt.json` and its sibling group were observed on disk from a real player
session for the first time**, along with the `Neural cold start:` line carrying a
real total. And **the Ada render-pace prior is now bracketed by measurement at
both ends - 0.897-0.923x at 1080p and 1.377-1.467x at 4K - which is why it was
left at 1.22**: it is the only value in that bracket that reproduces every
keeps-up verdict the machine's own measured pace implies.

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Microsoft Windows 11 Pro` / `10.0.26200` | `Win32_OperatingSystem`, recorded in each run's JSON |
| Shell | Windows PowerShell `5.1.26100.9444`, `-NoProfile` | `$PSVersionTable` |
| GPU | `NVIDIA GeForce RTX 4080 SUPER`, DXGI driver `32.0.16.1047` (610.47), generation `rtx40` | player startup line, `Win32_VideoController` |
| GPU (second adapter) | `Intel(R) UHD Graphics 770`, driver `32.0.101.6129` | `Win32_VideoController` |
| Adapter used | LUID `0x1600b`, vendor `0x10de`, 16047 MiB, named as the high-performance adapter | `D3D12 device adapter` log line |
| Repo commit | `0556eb0`, worktree `slice/session`, no `src/` modification at measurement time | `git rev-parse HEAD`, `git status` |
| Build | Release x64, VS 17 2022 / MSVC 19.44.35222.0, Windows SDK 10.0.26100.0 | configure log |
| Runtime | 12 locked files staged by `tools/stage_runtime.ps1`; `reshade=6.8.0.2155 renodx=4.7 nr=310.8.0` | receipt summary line |
| Runtime lock | `lock=ok`, 12 checks satisfied | `receipt.json` |
| Encoder | `hevc_nvenc` | run results |
| Media | `docs/media/neural-comparison-demo.mp4`, 1920x1080, 30 fps, 22.6 s, 678 frames | `ffprobe`, recorded in each run's JSON |
| Media (4K point) | the same clip rescaled to 3840x2160 at 15 fps with lanczos | `ffprobe` |
| Contention | none: the three concurrent slices acked idle and suspended their CPU scoring for the window; `tasklist` showed no `NeuralWorker.exe`, `DLSSVideoPlayer.exe` or `ffmpeg.exe` before the first batch | `hub` acks, `tasklist` |

The driver is at the neural floor (610.47), not the recommended pin (616.64), so
nothing here speaks for the pinned driver; it speaks for the floor.

## What the instrument does

`player_session.ps1 -Player <exe> -Media <clip> [-Sessions N]`, per session:

1. Notes the log's size and removes it. The player truncates it on the first
   `LOG()` anyway; when removal fails the pre-existing line count is noted and
   every parse skips past it.
2. Launches the player on the clip and polls `MainWindowHandle` until a visible
   window exists, timestamping the appearance.
3. Waits for the player's own `D3D12 device adapter` line, which is proof the
   clip is open and the renderer is up, then holds until `-ToggleAfterSeconds`
   past the window.
4. Injects the toggle as one `SendInput` batch and timestamps the keypress.
   `Ctrl+Alt+D` by default - the hotkey the player registers in
   `RegisterOverlayHotkeys`, delivered by `WM_HOTKEY` with no focus needed - or
   plain `D`, the key `docs/USAGE.md` documents, after forcing the foreground and
   verifying it with `GetForegroundWindow`. Both reach the same
   `ToggleNeuralRendering()`.
5. Scrapes the log for the session start, the first neural frame, the receipt,
   the published cache entry and the measured pace, then reads `receipt.json`
   off disk and lists its sibling group.
6. Sends the toggle again to stop the session, which is what makes the player
   report its measured pace, closes the window with `WM_CLOSE`, and emits JSON
   with every interval, every phase, and the raw log line each number came from.

Three properties are load-bearing:

- **Every post-toggle wait is windowed to the keypress.** A player that opens a
  clip runs a cache-check job first, and that job logs its own
  `Neural cold start:` line *before* the toggle. The first version of this script
  matched it and reported a neural frame presented 3 s before the key went in.
  Post-toggle waits now start at the keypress, and the cold-start pattern
  additionally requires a numeric `total=`, because
  `NeuralColdStartRecord::Presented` is the only thing that records a total - so
  `total=-` is a job that never put a frame on screen.
- **A missed pattern is never a fast session.** Each failure mode exits with its
  own code (`0` pass, `2` arguments, `3` no window, `4` player exited, `5`
  refused, `6` unanswered, `7` no neural frame, `8` no pace, `9` modal dialog,
  `10` cold start without attach, `11` no foreground, `12` second instance, `13`
  injection refused, `14` locked desktop) and `outcome.ok` is in the JSON.
- **The clip is probed first.** A photo takes the same toggle and renders a
  single frame instead of starting a session, and a clip under 8 s can never
  reach the 120 rendered frames the player needs before it reports a pace.
  Either would come out of the scrape as "the key did not arrive", which is a
  lie about the instrument rather than about the clip.

Both clocks are the system clock - `GetLocalTime` in `src/Log.h`, `Get-Date`
here - so the difference of two of them carries no cross-clock skew, at that
clock's ~15.6 ms tick.

## Gates

- `ctest --test-dir build-upscaling -C Release -R PolicyTests`: **1/1 passed**,
  21.82 s, with the Ada-bracket test below added.
- **Ten sessions on an idle GPU, all exit 0**, every one rendering the real
  neural path: `feature18=armed`, `lock=ok`, `failure=none`,
  `verified_neural_frames` equal to `frame_count`, zero frame retries.
- Two independent clocks cross-checked per session: wall-clock keypress-to-log
  against the player's own `total=`, agreeing to 8-19 ms in ten of ten.
- Nothing else on the GPU or the CPU for the window: three peer slices acked
  idle and suspended their scoring.

## Toggle to first neural frame

The number P1's acceptance criterion is written against. *Cold* means
`DLSSVideoPlayer.ini` and the whole `cache/` tree removed first, so the pace
profile and the preflight verdict are both absent - a fresh install. *Warm*
keeps both and drops only `cache/v1/renders`, because a live session whose render
key is already cached publishes that entry without rendering a segment and never
presents anything (see the findings below).

| # | state | keypress → first neural frame (s) | player's own `total=` (s) | delta (ms) |
| --- | --- | --- | --- | --- |
| 1 | cold 1080p30 | 8.389 | 8.371 | 18 |
| 2 | cold 1080p30 | 9.180 | 9.166 | 14 |
| 3 | cold 1080p30 | 8.882 | 8.867 | 15 |
| 4 | warm 1080p30 | 5.162 | 5.143 | 19 |
| 5 | warm 1080p30 | 5.149 | 5.140 | 9 |
| 6 | warm 1080p30 | 4.884 | 4.876 | 8 |
| 7 | warm 1080p30 | 5.022 | 5.013 | 9 |
| 8 | warm 1080p30¹ | 4.995 | 4.983 | 12 |
| 9 | cold 4K15 | 11.994 | 11.980 | 14 |
| 10 | cold 4K15 | 12.477 | 12.466 | 11 |

¹ Session 8 ran with two geometries in its pace profile rather than one, which
changes the predicted realtime ratio and so could change the start lead. Its
number falls inside the other four warm sessions' range, so it is reported with
them and flagged rather than silently pooled.

**Cold 1080p30, 3 sessions: 8.389 / 8.882 / 9.180 s** - median 8.882, mean 8.817,
spread 0.791 s.
**Warm 1080p30, 5 sessions: 4.884 / 4.995 / 5.022 / 5.149 / 5.162 s** - median
5.022, mean 5.042, spread 0.278 s.

These are wall-clock, unattended, from a real keypress to a real neural frame on
a real screen. They are not reproducible the way the render metrics are: a render
is bit-identical between repeats, a cold start is not, and the spread above is
the honest width of it.

## Where the time goes

Four of the nine cold-start phases are the player's own, and only a player
session can measure them. They come from the `Neural cold start:` line the player
writes at the attach; the helper's five reach that line too late to appear in it
(see the findings) and were read out of `receipt.json`.

Player-side phases, from the log line, all ten idle sessions:

| phase | cold 1080p (3) | warm 1080p (5) | cold 4K (2) | what it covers |
| --- | --- | --- | --- | --- |
| `request` | 0.072-0.091 s | 0.077-0.085 s | 0.093 / 0.099 s | keypress → the job's own work begins |
| `preflight` | **3.511 / 3.734 / 3.858 s** | absent, all five | 3.453 / 3.495 s | the feature-18 probe helper, a whole second process |
| `launch` | 0.005 s | 0.004-0.005 s | 0.005 s | → the render helper's process created |
| `attach` | 1.268 / 1.279 / 1.304 s | 1.244-1.392 s | 2.108 / 2.365 s | first playable output in hand → first neural frame presented |

`preflight` absent on a warm session is the absence ladder working as designed:
the probe did not run because its verdict was already in `cache/v1/preflight`,
and that is a different claim from a probe that took no time. **That single
phase is the whole cold-warm difference**: 3.701 s mean measured preflight
against a 3.775 s mean difference in the totals.

The complete nine-phase timeline, from the two `receipt.json` files that survived
the batches - one warm 1080p session, one cold 4K session, both idle:

| phase | warm 1080p30 (s) | cold 4K15 (s) | harness A / B, same machine² |
| --- | --- | --- | --- |
| `request` | 0.077188 | 0.092505 | - / - |
| `preflight` | null | 3.495372 | - / - |
| `launch` | 0.003934 | 0.005038 | - / - |
| `helperStart` | 0.101020 | 0.102079 | 0.1044 / 0.099 |
| `runtimeReady` | 0.010186 | 0.011355 | 0.0102 / 0.010 |
| `neuralInit` | 1.415179 | 1.785503 | 1.3385 / 1.847 |
| `featureArm` | 0.688672 | 0.748212 | 0.6805 / 0.641 |
| `firstOutput` | 1.413875 | 3.837052 | null / 2.726 |
| `attach` | 1.244121 | 2.364627 | - / - |
| **`total`** | **4.982788** | **12.466287** | - / - |

² `docs/VERIFICATION-2026-09-14-RTX4080.md`, the same machine through the
headless harness. The five helper phases agree closely with it, which is the
cross-validation this record can offer: the harness and the player see the same
helper. The four player-side phases and the total are new.

**What this says about P1.** Of the 8.817 s mean cold toggle-to-picture at
1080p30, `neuralInit` + `featureArm` is 2.10 s and every millisecond of it is
per-process - a resident helper removes it from the second toggle onward. The
preflight probe is another 3.70 s, and it is already amortised: paid on the first
toggle after an install and never again for that runtime identity, which is why
the warm number is 5.04 s. What a resident helper cannot remove is `firstOutput`
(1.41 s: the render must finalize a whole segment before anything is playable)
and `attach` (1.24 s: the player's own work opening the pair, seeking it and
presenting it). Those two are 2.66 s of the warm 5.04 s, so on the arithmetic of
these phases **a resident helper takes the warm toggle-to-picture from ~5.0 s to
~2.9 s at 1080p30 on this machine** - it does not take it below the
segment-plus-attach floor. That projection is arithmetic on measured phases, not
a measurement of a resident helper.

## The Ada render-pace prior: measured at both ends, and left alone

`RenderPacePrior(Rtx40Ada)` is `kAdaRenderPacePrior = 1.22`, taken from a single
1080p30 live session on this same machine and driver on 0.16.0 - 15.31 ms/frame
over 738 frames. Its comment says the prior "is high until" an Ada machine
measures the pipelined loop. One now has, by the same segment-arrival method, at
two geometries:

| geometry | sessions | ms/frame | x the reference model | rendered frames |
| --- | --- | --- | --- | --- |
| 1920x1080 | 8 | 11.2482 - 11.5734, mean 11.4222 | 0.8974 - 0.9234, mean 0.9113 | 582-592 each |
| 3840x2160 | 2 | 38.6853 / 41.2035, mean 39.9444 | 1.3774 / 1.4671, mean 1.4222 | 293 / 292 |

The 1080p spread is 0.325 ms, 2.9 %, over eight sessions; the 4K spread is
2.52 ms, 6.3 %, over two. The 1080p figure confirms what the comment expected:
11.42 against 15.31 ms/frame is 25 % off the 0.16.0 session, close to the 29 %
the pipelined loop took off 1080p on Blackwell.

**The prior was not changed, and the 4K row is why.** It is one scalar applied at
every geometry, and the two measured ends bracket it. Forecasting the five
geometries that matter against what this machine's own measured pace implies
(both samples in the profile, so the player fits their line):

| candidate prior | 1080p30 | 1080p60 | 1440p30 | 1440p60 | 4K30 |
| --- | --- | --- | --- | --- | --- |
| measured truth | ok 2.918x | ok 1.459x | ok 1.771x | warn 0.886x | warn 0.834x |
| 0.91, the 1080p scale | ok 2.922x | ok 1.461x | ok 2.211x | **ok 1.106x** | **ok 1.304x** |
| **1.22, unchanged** | ok 2.180x | ok 1.090x | ok 1.649x | warn 0.825x | warn 0.973x |
| 1.38, the 4K scale | ok 1.927x | **warn 0.964x** | ok 1.458x | warn 0.729x | warn 0.860x |

Lowering the prior to the measured 1080p scale would make a fresh Ada install
promise 4K30 and 1440p60 - it predicts 25.56 ms where the machine measures
38.69-41.20 - and a session started on that promise renders at 25 fps against a
30 fps playhead. Raising it to the measured 4K scale would make the same install
refuse 1080p60, which this machine runs at 1.46x realtime. **1.22 is the only
candidate that reproduces all five verdicts**, so the measurement that was
expected to lower it is the reason it stands. It still understates the 4K cost by
14 %; that is inside the margin the 0.98 keeps-up threshold leaves, and the first
session at any geometry replaces the prior with that geometry's own pace anyway.

`tests/PolicyTests.cpp` now pins both ends of the bracket - against the real
`RenderPacePrior(Rtx40Ada)`, and again against the slowest 1080p and fastest 4K
samples directly - so a later "update the prior to the measurement" cannot
quietly flip either verdict. The stale half-sentence in
`src/RuntimePolicy.cpp`'s comment, "no Ada machine has measured the pipelined
loop ... so the prior is high until one does", is now false; this slice does not
own that file and did not touch it.

## The receipt group and the cold-start line, from a real player session

Both had unit coverage only. Observed here, cold 1080p session 1:

```
[16:53:39.788] Active neural session started at 2.66667 s through 22.6 s.
[16:53:48.159] Neural cold start: total=8.371s request=0.085s preflight=3.511s launch=0.005s helperStart=- runtimeReady=- neuralInit=- featureArm=- firstOutput=- attach=1.268s
[16:53:48.159] Active neural playback attached at 2.66667 s with 2.49996 s buffered.
[16:53:55.237] Neural render receipt: gpu="NVIDIA GeForce RTX 4080 SUPER" driver=32.0.16.1047 reshade=6.8.0.2155 renodx=4.7 nr=310.8.0 feature18=armed lock=ok failure=none frames=598/598 verified=598 resets=9 retries=0 cuts=8 suppressed=1
[16:53:58.830] Active neural session rendered 598 frames and published its cache entry; save=1 entry=...\cache\v1\renders\aec66460...\neural.mkv
[16:54:02.072] Measured neural render pace: 1920x1080 at 11.5087 ms/frame over 583 frames (0.918199x the reference GPU); 1 geometries known for this GPU.
[16:54:02.077] Active neural session stopped at 16.4491 s; presented=414 dropped=0
```

The group on disk beside `neural.mkv`, checked by the script in every passing
session, ten of ten:

| file | bytes |
| --- | --- |
| `manifest.json` | 811 |
| `neural-settings.ini` | 241 |
| `receipt.json` | 7540-7556 |
| `neural.mkv` | ~14.6 MB |

`receipt.json` parses and carries `schema`, `jobId`, `renderKey`,
`settingsDigest`, `runtimeDigest`, `started`, `finished`, `request`, a non-null
`preflight` object, `lock` with 12 satisfied checks, and `result` with the full
`coldStartMicroseconds` group tabulated above.

## Two findings the instrument turned up

**A live session that hits the render cache never presents a frame.** Toggle on,
let the session publish, toggle off, toggle on again at the same playhead: the
render key matches the published entry, the job returns a cache hit, the segment
index stays empty, `ShouldAttach` sees a finished session with zero lead and
never fires, and the player sits behind the buffering panel indefinitely.
Reproduced deliberately, and diagnosed by the script in 30 s instead of by a 90 s
timeout:

```
Active neural session started at 2.5 s through 22.6 s.
Checking neural cache key=202c15b3... range=[27000000,226000000) guides=mv=1,depth=1 ...
Active neural session rendered 597 frames and published its cache entry; save=0 entry=...\202c15b3...\neural.mkv
Neural cold start: total=- request=0.120s preflight=- launch=- helperStart=- runtimeReady=- neuralInit=- featureArm=- firstOutput=- attach=-
```

`total=-` is the player correctly reporting that nothing was ever presented. The
fix belongs in `src/main.cpp`, which this slice does not own; `-DropRenderCache`
exists so that repeated measurement does not walk into it.

**The log's cold-start line structurally cannot carry the helper's phases.**
`coldStart->Merge(completion->result.coldStart)` runs at `src/main.cpp:3616`,
after `RunNeuralWorker` returns - seconds after the attach, where
`NoteNeuralFramePresented` writes the line, and `ClaimReport` makes that
once-only. So a live session's `Neural cold start:` line reads
`helperStart=- runtimeReady=- neuralInit=- featureArm=- firstOutput=-` in all ten
sessions, while `receipt.json` for the same render carries all five. Both are
recorded above; neither is derived from the other. This is an ordering, not
missing data, but a reader of a user's log should know the helper's phases only
ever land in the receipt.

## Failure modes exercised while building this

Every one was produced on this machine, not reasoned about:

| code | mode | how it arose |
| --- | --- | --- |
| 10 | a cold-start line with no attach following it | the pre-toggle cache-check job's own `Neural cold start: total=-` line was matched; the reason the waits are windowed and the total must be numeric |
| 11 | the player window would not take the foreground | two sessions in four with `-Accelerator D`: Windows refuses `SetForegroundWindow` to a process that did not receive the last input. The reason `Ctrl+Alt+D` is the default |
| 7 | no first neural frame | the render-cache hit above, reproduced deliberately |
| 3 | no visible main window | `-WindowTimeoutSeconds 0.01` |
| 2 | media a session cannot use | pointed at `neural-comparison-poster.jpg`: 0.04 s, refused before any player was launched |
| 2 | missing player | a path that does not exist |
| 12 | a second instance already running | a player started by hand, then the script |
| 14 | injected input refused | the workstation locked at 17:04: `SendInput` returns `ERROR_ACCESS_DENIED` and `GetForegroundWindow` returns null |
| 6 | the toggle produced no log response at all | once in fourteen sessions on `Ctrl+Alt+D`, cause undetermined - the hotkey was registered, the clip was playing, and neither a session line nor a refusal line followed. Plausibly the start of the same lock transition, but not proven |
| 9 | a modal dialog blocked the session | two sessions after a contended one had written 42.3 ms/frame into the pace profile: the forecast decided the card could not keep up and raised `ConfirmLiveSessionPace`, `#32770` titled "Neural rendering from here". Detected in seconds instead of waiting out a 90 s timeout against a dialog nobody would ever click |

Code 6 is the reason the script presses once more, and only into silence: a press
that arrived is visible in the log within the same message-loop turn, so a grace
window with neither a session line nor a refusal line means nothing ran and a
second press cannot toggle anything off. A refusal is never retried, and neither
is a started session. The attempt count travels with the numbers, so a retried
session is never silently equal to a clean one; all ten citable sessions took one
attempt.

An up-front locked-desktop guard was written and then removed rather than
shipped. `OpenInputDesktop` is the textbook check and it is wrong here, because
the modern lock screen is a protected window on the ordinary desktop: it reported
"available" while injection was refused, and the first chord was even accepted
and went nowhere. A check that cannot detect the condition it names is worse than
no check.

The replacement is not a predicate either, and for a second measured reason:
while the lock screen is up, `GetForegroundWindow` **flaps**. Polled eight times
at 700 ms on the locked machine it returned null twice and the lock-screen window
six times, and one unlucky single sample is what made this record briefly claim
the box had unlocked. So the foreground is only consulted after `SendInput` has
already been refused, it is sampled five times, and any null decides it -
otherwise the same locked machine would be classified `13` or `14` depending on
which phase of the flap it happened to catch.

## What contention costs, and what one contended session costs afterwards

Sessions taken while other work was on the machine, kept only to show why the
quiet window was worth asking for. **CONTENDED - not citable**, and not carried
into any table above.

| condition | toggle → picture | pace at 1080p |
| --- | --- | --- |
| idle (the record) | 8.39-9.18 s cold | 11.25-11.57 ms/frame |
| three slices rendering | 10.220 s, 11.013 s | 14.5231, 16.7275 ms/frame |
| peer `analyze.py` decodes only | 8.865 s | 11.0327 ms/frame |
| peer decodes saturating the CPU | - | 42.333431 ms/frame³ |

³ Not read from a log line but from the `[NeuralPace]` sample that session wrote
into `DLSSVideoPlayer.ini`, which is the same number `RecordLiveRenderPace`
logs. 3.7x the idle mean. The mechanism was first written up here as CPU
contention with the GPU free; a later controlled test contradicted that and the
attribution is withdrawn. Saturating all 20 threads at a verified 100 % moves
1080p pace only 11.4 -> 15.5 ms/frame, 1.36x, because a live session's pace is
GPU-dominated - so the 42.3 ms sample almost certainly carried GPU and NVENC
contention from the three slices rendering that evening as well. The mechanism
is a mix of undetermined proportions; CPU alone is measured insufficient.

**And that sample is what produced the modal dialog.** The profile keeps one
sample per geometry and the newest replaces the oldest, so a single session
measured under load leaves 42.3 ms/frame on disk as this machine's 1080p pace.
The next two sessions decided the card could not keep up and raised
`ConfirmLiveSessionPace` - "Neural rendering from here" - on hardware that had
just rendered the same clip faster than realtime. The dialog and its two exit-9
sessions are observed; the figures it would have shown are NOT. Those sessions
ran without log capture and the next launch truncated the log, so "23.6 fps
against a 30 fps playhead" and "2.9x realtime" are arithmetic from the stored
sample (1000/42.333 and 1000/11.422 over 30), not readings. They are quoted here
as a calculation and must not be cited as measurements.
Nothing recovers the profile except another session, which the
user now has to click through a warning to start. That is a product finding, not
an instrument one: `RenderPaceProfile::Record` has no notion of a sample taken
under load and no way to distrust one.

## What this record does not establish

- **The committed script completed its success path, contended.** The
  workstation unlocked long enough for one full session on the exact bytes in
  this commit: exit 0, one toggle attempt, keypress to first neural frame
  8.865 s, `receipt.json` on disk with 12 satisfied lock checks and all nine
  phases, clean `WM_CLOSE` exit. Peer decodes were running, so **that 8.865 s is
  contended and not citable** and does not restate any of the ten sessions above
  - but it lands inside the published cold band of 8.389-9.180 s and its phase
  structure matches, which is a free cross-check. It also supplied the one
  complete nine-phase timeline this record lacked for a *cold* 1080p session:
  `request` 0.079771, `preflight` 3.689307, `launch` 0.008430, `helperStart`
  0.109586, `runtimeReady` 0.011555, `neuralInit` 1.407146, `featureArm`
  0.732614, `firstOutput` 1.398813, `attach` 1.392544, `total` 8.843465 s.
- **UNEXERCISED: the resident helper.** P1 itself is not implemented here, and
  the ~2.9 s projection above is arithmetic on measured phases.
- **UNEXERCISED: a scanned first-touch install.** Every session ran against a
  runtime tree already executed repeatedly in the same session, so `helperStart`
  at 0.10 s is a warm-cache number. Real-time protection is reported enabled, but
  the exclusion state cannot be read without administrator rights.
- **UNEXERCISED: 4K30 playback.** The 4K point is a 15 fps clip, chosen so the
  session starts without `ConfirmLiveSessionPace` asking. It measures the
  per-frame cost of that geometry, which is what the prior needs; it is not a
  claim that 4K30 plays. On the measured pace it does not.
- **UNEXERCISED: 1440p.** The 1440p column in the prior table is the player's own
  two-sample fit between the two measured geometries, not a measurement.
- **UNEXERCISED: four of the fourteen exit codes.** Ten were produced on this
  machine and are tabulated above. The four that were not: `4` the player
  exiting mid-session, `5` the player refusing a toggle it received (the
  refusal lines exist in `src/main.cpp` and are matched, but no session put the
  player in a state that emits one), `8` a session stopping without reaching
  120 rendered frames, and `13` an injection refusal with no locked desktop or
  higher-integrity foreground to explain it - which, by construction, is the
  case nobody knows how to produce on purpose.
- **UNEXERCISED: plain `D` as the unattended accelerator.** It worked in two
  sessions and was refused the foreground in two others, so all ten citable
  sessions used `Ctrl+Alt+D`. Both reach the same `ToggleNeuralRendering()`, but
  no citable number was taken through `WM_KEYDOWN`.
- **Not the pinned driver.** 610.47, not 616.64.
- **One machine, one clip.** Ten sessions on one Ada desktop against one 1080p30
  clip and its 4K rescale. Nothing here speaks for Turing, Ampere, Blackwell, a
  laptop, or a hybrid-graphics machine.
- **Timing, not quality.** No quality metric was computed. The renders were
  checked for `failure=none` and `verified == frames`, nothing more.

## Reproducing

From the slice worktree, with the runtime staged into
`build-upscaling/Release/neural-runtime` and the workstation unlocked:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verification/player_session.ps1 `
  -Player build-upscaling/Release/DLSSVideoPlayer.exe `
  -Media docs/media/neural-comparison-demo.mp4 `
  -Sessions 3 -FreshProfile Each -RunSeconds 60 `
  -OutJson build-upscaling/session-runs/cold.json -Note "GPU idle"

powershell -NoProfile -ExecutionPolicy Bypass -File tools/verification/player_session.ps1 `
  -Player build-upscaling/Release/DLSSVideoPlayer.exe `
  -Media docs/media/neural-comparison-demo.mp4 `
  -Sessions 4 -FreshProfile Never -DropRenderCache -RunSeconds 60 `
  -OutJson build-upscaling/session-runs/warm.json -Note "GPU idle"
```

That is four of the five warm sessions. The fifth is session 1 of the
cache-hit reproduction below, which is the same warm configuration except that
the pace profile already held the 4K sample as well; it is the session marked
with a footnote in the table above.

The 4K point used the same clip rescaled with the staged FFmpeg:

```
build-upscaling/Release/ffmpeg.exe -i docs/media/neural-comparison-demo.mp4 `
  -vf "scale=3840:2160:flags=lanczos,fps=15" -c:v libx264 -preset veryfast -crf 20 `
  -pix_fmt yuv420p -c:a aac build-upscaling/session-runs/uhd15.mp4
```

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verification/player_session.ps1 `
  -Player build-upscaling/Release/DLSSVideoPlayer.exe `
  -Media build-upscaling/session-runs/uhd15.mp4 `
  -Sessions 2 -FreshProfile Each -RunSeconds 90 -NeuralFrameTimeoutSeconds 120 `
  -OutJson build-upscaling/session-runs/uhd.json -Note "GPU idle"
```

And the cache-hit finding, which is the same invocation with `-DropRenderCache`
left off so the second session meets its own published entry:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verification/player_session.ps1 `
  -Player build-upscaling/Release/DLSSVideoPlayer.exe `
  -Media docs/media/neural-comparison-demo.mp4 `
  -Sessions 2 -FreshProfile Never -RunSeconds 60 -NeuralFrameTimeoutSeconds 40 `
  -OutJson build-upscaling/session-runs/cachehit.json -Note "GPU idle"
```

Each run writes its JSON and one copy of every session's log beside it, under
`build-upscaling/`, which is not tracked. The numbers above are therefore quoted
here with the log line each was read from, rather than by reference to a file a
clean checkout would not have. `tools/verification/log_matrix_row.py` turns any
one of those logs into a matrix row.
