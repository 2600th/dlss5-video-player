# Idle VRAM policy A/B, and the crash/device-removal recovery - 14-15 September 2026, RTX 4080 SUPER

**Verdict: P3 is measured and the default is chosen on evidence - keep feature
memory. Policy B (`free`) frees 361 MiB of the 1061 MiB a resident helper holds
while idle, and costs 0.604 s on every reuse - 25 % of the reuse latency that P1
exists to produce (n=3 per arm as of 2026-09-15; this verdict first shipped at
n=2, reading 0.70 s / 28 %).** Both arms ran on a real driven player session on
this machine, both arms printed the policy they were actually running, and the
release genuinely happened - `released=1`, `featureArmed=0`, `observed=freed`.
So this is a measured trade, not a failed release.

What it does not establish: nothing here speaks for a runtime that declines to
give the memory back (the `observed=not-freed` branch was never taken on this
driver), nothing here is a 5-minute idle sample, and nothing here was measured
on a card small enough for 361 MiB to matter. Those three gaps are recorded
below rather than argued around.

P6 (helper crash / device-removal fast recover) is proven by tests that drive
real helper processes, not by a reasoned argument about the code path: three
cases in `tests/NeuralWorkerTests.cpp`, all passing in the full 13-suite ctest
run.

## Environment

| Item | Value | Evidence |
| --- | --- | --- |
| GPU | NVIDIA GeForce RTX 4080 SUPER, 16047 MiB, driver `32.0.16.1047` (610.47) | player startup line, preflight receipt |
| CPU / OS | Intel Core i7-14700, Windows 11 Pro `10.0.26200` | `Win32_OperatingSystem` |
| Runtime | ReShade `6.8.0.2155`, RenoDX `4.7`, DLSS-NR `310.8.0` | receipt summary line |
| Instrument | `tools/verification/player_session.ps1` - launches the real player, injects the neural toggle through `SendInput`, scrapes `DLSSVideoPlayer.log` | `docs/VERIFICATION-matrix.md` |
| Clip | 90 s, 1080p30 | run JSON |
| Policy selection | `DLSSVideoPlayer.ini` beside the executable, `[NeuralHelper] IdleVramPolicy=keep|free`, read by `ReadIdleVramPolicy(PlayerSettingsPath())` at `src/NeuralWorker.cpp:1239-1253` | helper log line, per arm |
| Idle grace | 5 s (`resident_worker::kIdleSampleGrace`, `src/ResidentWorkerLoop.h:46`) against the 30 s idle exit budget (`:37`) | source |

The driver is at the neural floor (610.47), not the recommended pin, so these
numbers speak for the floor.

Renders are bit-identical across repeats within an arm - verified again today by
decoded-frame sequence digest - so the render side of each session is not a
source of the spread below. The toggle-to-picture side is not bit-identical and
never has been; the two sessions per arm are the honest width of it.

**Third pair taken 2026-09-15: n=3 per arm, and the seven-tenths was a little
high.** The first attempt met a locked workstation - exit 14 on every session, the
harness naming `WTSSessionInfoEx SessionFlags = WTS_SESSIONSTATE_LOCK` rather than
inferring it - so the headline stood at n=2 for a day. Re-run on an unlocked box
with three sessions per arm, every session asserted from the helper's own log:

| arm | reuse seconds (second toggle, plan=reuse) | median | idle VRAM |
|---|---|---|---|
| `keep` | 2.400, 2.326, 2.405 | **2.400** | `localVramMiB=1061 featureArmed=1`, no release |
| `free` | 3.004, 3.089, 3.002 | **3.004** | `beforeMiB=1061 localVramMiB=700 freedMiB=361 released=1 observed=freed`, all three |

So `free` costs **+0.604 s per reuse, +25.2 %**, against the +0.70 s / +28 % the two
-sample pair reported. Ranges still do not overlap, by a wider margin than before
(2.326-2.405 against 3.002-3.089), and the 361 MiB reproduces exactly three times
out of three. The first toggle of each session is indistinguishable between arms
(keep 4.896-5.080, free 4.876-5.008), which is what the mechanism predicts: the
release is paid on the next reuse, not on the session that performs it.

The evidence is beside this file rather than in `%TEMP%`, where the first pair's
still lived until today: `sessions/n3-{keep,free}.json` are the harness records and
`sessions/n3-<arm>.sessionN.helper.log` the six helper logs the assertions read.
`sessions/n2-{keep,free}.json` are the 2026-09-14 pair, kept because this report
quotes its numbers. A `%TEMP%` path is not a record - it survives until the next
cleanup, and the stem `p3-<arm>` would have been reused by any later run.

Every one of the six sessions was proven to be the arm it was labelled, from
`idleVramPolicy=` in the helper's own log rather than from the run's label - the
failure this measurement hit on its first attempt, when
`player_session.ps1` deleted the ini that selects the policy and both arms silently
ran `keep`. The harness now collects that evidence itself: each session records a
`helperLogCopy`, and refuses to copy a helper log whose write time did not move, so
a session that started no helper cannot inherit the previous one's lines. All six
read `outcome=copied`.

That failure is incidentally the first live proof of the lock probe shipped the
same day: the pre-probe harness would have reported this as an inference from a
flapping foreground window, and it named the session flag instead.

## The instrument line

```
tools/verification/player_session.ps1 -SecondToggle -SecondToggleSeekBacks 7 `
  -FreshProfile Never -DropRenderCache -Sessions 2
```

`-FreshProfile Never` is load-bearing and has its own section below.
`-DropRenderCache` keeps the pace profile and the preflight verdict while
dropping `cache/v1/renders`, so every session renders a real segment instead of
publishing a cache hit and presenting nothing. `-SecondToggle` with seek-backs
is what produces the reuse number P1's acceptance criterion is written against.

## Policy A - `keep`, the shipped default

Helper log, `neural-runtime\DLSSVideoPlayer.log`:

```
Resident helper session starting with idleVramPolicy=keep
Neural helper post-job VRAM: job=2 localVramMiB=1061 featureArmed=1 idleVramPolicy=keep
Resident helper idle VRAM: policy=keep localVramMiB=1061 featureArmed=1
Neural helper post-job VRAM: job=4 localVramMiB=1061 featureArmed=1 idleVramPolicy=keep
```

The helper parks 1061 MiB with feature 18 still armed, and the next job finds
exactly that - post-job at job 4 is the same 1061 MiB, because nothing was given
back and nothing had to be re-armed.

## Policy B - `free`

```
Resident helper session starting with idleVramPolicy=free
Neural helper post-job VRAM: job=2 localVramMiB=1061 featureArmed=1 idleVramPolicy=free
Resident helper idle VRAM: policy=free beforeMiB=1061 localVramMiB=700 freedMiB=361 released=1 featureArmed=0 observed=freed
Neural helper post-job VRAM: job=4 localVramMiB=1061 featureArmed=1 idleVramPolicy=free
```

1061 MiB down to 700 MiB, 361 MiB given back, feature disarmed. The 700 MiB that
stays is the retained device and its non-feature allocations, which is the whole
point of the arm: B releases the feature workset and keeps the device, so the
next job re-arms rather than paying `neuralInit` again
(`src/OfflineNeuralRenderer.cpp:1796-1812`). The post-job line at job 4 is back
at 1061 MiB, which is the re-arm having happened. That re-arm is visible in the
residency classification too: a job that inherited the device but re-armed the
feature reports `FeatureRecreated`, not `FeatureReused`
(`src/OfflineNeuralRenderer.cpp:2463-2472`), so B's cost is not silent.

`observed=freed` is the adapter's own answer, not a claim that the release
worked - the code records a refusal as a refusal
(`src/NeuralWorkerMain.cpp:437-447`). On this driver it did not refuse.

## What B costs: the reuse latency, both arms

Second-toggle reuse latency, the P1 acceptance number, same instrument, two
sessions per arm:

| arm | session 1 (s) | session 2 (s) | median (s) |
| --- | --- | --- | --- |
| `keep` (default) | 2.477 | 2.543 | 2.510 |
| `free` | 3.239 | 3.195 | 3.217 |

**+0.604 s median on a 2.400 s baseline, +25.2 % (n=3 per arm, 2026-09-15;
the two-sample pair read +0.70 s / +28 %), in exchange for 361 MiB - 34 % of
the 1061 MiB an idle helper holds.** The two arms do not overlap: the slowest
`keep` session is 0.597 s faster than the fastest `free` session (2.405 against
3.002), so neither the direction nor - at three sessions per arm - the magnitude
rests on a single pair. At n=2 that same gap was 0.65 s (2.543 against 3.195).

## The conclusion in the terms the roadmap asked for

Both policies are viable, and the choice is a trade with both sides measured.

**A ships** because reuse latency is the feature's entire purpose. Residency
exists to take the warm toggle from ~5.0 s to 2.44-2.52 s
(`docs/VERIFICATION-matrix.md`); spending 28 % of that back to reclaim a third
of the idle footprint inverts the reason the work was done.

**B is the answer for a card where 361 MiB decides whether a second application
fits**, and it is one ini key away: `[NeuralHelper] IdleVramPolicy=free` beside
the executable, no rebuild, no flag on the command line the user never types.
The key is deliberately the player's own ini and not the runtime's
`ReShade.ini`, so it never enters the neural-settings digest the render cache key
hashes - a policy change does not invalidate a cached render, and `PlanForJob`
still answers `Reuse` across one.

Not measured: whether 0.604 s is the cost on any other card, driver or clip. One
machine, one driver, one 1080p30 clip, three sessions per arm.

## A trap in the instrument: the two arms were secretly the same arm

The first attempt at this measurement produced an A/B in which both arms ran
policy A. `player_session.ps1:785` deletes `DLSSVideoPlayer.ini` when it clears
the profile:

```powershell
if (Test-Path -LiteralPath $iniPath) { Remove-Item -LiteralPath $iniPath -Force; $removed += 'DLSSVideoPlayer.ini' }
```

That is exactly what `-FreshProfile First|Each` is supposed to do before a cold
session - a fresh install has no ini. But the policy is selected *through* that
ini, so an ini written before launch is gone by the time the player reads it,
and both runs silently used `kDefaultIdleVramPolicy`, which is `keep`.

The tell was in the helper log, not in the timings: the run labelled `free`
printed

```
Resident helper session starting with idleVramPolicy=keep
```

and its idle line took the `policy=keep` shape with no `freedMiB=` at all. The
timings, read on their own, looked like a clean null result - two arms, no
difference, "the policy does nothing". That would have been a false negative
published as a finding.

The fix is `-FreshProfile Never`, which keeps the ini and drops only the render
cache. The general lesson is the one this project keeps relearning: **a policy
A/B needs a per-arm assertion that the arm actually took effect, not just a
per-arm label.** Here that assertion already existed in the implementation - the
session-start line names the policy in force - and reading it is what turned a
wrong measurement into a right one. Any future arm of this A/B should be
rejected if its helper log does not name the policy the arm claims.

## P6: crash and device-removal recovery, proven on real helper processes

Implemented against the existing "relaunch at most once" semantics: on helper
death or TDR the parent restarts the helper once, re-runs preflight before
handing it work again, and resumes; a second failure fails closed with a reason;
no orphan process is left on either path. The recovery probe is wired into both
launchers (`src/NeuralWorker.cpp:785-819` for the probe,
`:1140-1143` and `:1285-1291` for the two launch sites), so the benchmark harness
inherits it as well as the player, and it is bounded by the same relaunch limit
so nothing can spin.

Three cases in `tests/NeuralWorkerTests.cpp`, all passing in the full 13-suite
ctest run:

- **`killed_resident_helper_restarts_once_and_completes_the_job_test`** - the
  first helper calls `TerminateProcess` on itself from inside `Job()`. Asserts
  the job completes, the `jobId` is preserved across the restart, exactly 2
  helper processes took the job, and exactly 1 segment restart (the range was
  re-rendered from frame zero rather than resumed mid-segment).
- **`second_kill_fails_closed_with_a_reason_and_leaves_no_orphan_test`** - the
  helper dies every time. Asserts `RetryExhausted`, a detail containing
  `did not recover after 2 attempt(s)`, the helper no longer resident, and zero
  live child processes.
- **`recovery_probe_refusal_fails_closed_instead_of_relaunching_test`** - the
  helper dies once and the preflight probe is refused. Asserts
  `NeuralRenderFailure::Preflight`, the probe's own reason carried through into
  the detail, and exactly 1 helper process - i.e. the single relaunch was never
  spent on a machine that cannot render.

The orphan check is stronger than "the job object is empty": `LiveChildProcesses()`
walks a `CreateToolhelp32Snapshot` and counts processes whose parent is the test
process and whose handle is still unsignalled, so it also catches a helper that
escaped its job object. It is applied after the kill-once path, the kill-twice
path, the probe-refusal path and both idle-VRAM policy runs, and it is
load-bearing: one mutation run left a helper resident and the check reported
non-zero.

No new compile definition and no build-graph change was needed. The test binary
re-executes itself as the helper and the request's source file name selects the
injected failure - the pattern already inside that test file.

## Wire and receipt surface

`kVersion` stayed at **6**. The change is additive: `WireKind::Memory = 7` with a
fixed 16-byte `WireMemory` payload (`src/NeuralWorkerProtocol.h:45-52`,
`:149-171`, `:180`), and `WireResult` is untouched at 152 bytes with its
`static_assert` intact.

The Python decoder in `tools/benchmark/common.py` needed no change and was not
touched: it advances by the header's payload length before dispatching on kind
and silently skips kinds it does not name, the same way it already skips
`Ready = 6`.

Three receipt fields were added inside the existing `timing` group
(`src/NeuralReceipt.cpp:244-253`):

| field | meaning |
| --- | --- |
| `postJobLocalVramMiB` | local VRAM with the render finished and the device quiet |
| `idleLocalVramMiB` | local VRAM after the idle grace elapsed, post-release under `free` |
| `idleVramPolicy` | the arm that produced the two samples, `"keep"` or `"free"` |

**`idleLocalVramMiB` is 0 when no idle period preceded the job** - the first job
a process serves, and every single-shot render. That is an absent measurement,
not a measured zero, and it is only ever to be read next to a run that had an
idle period to measure.

`--idle-vram-policy` is accepted on a resident helper command line only. It is
rejected on a single-shot or preflight line, because a single-shot helper exits
instead of idling, so the flag would name a behaviour that cannot occur.

## The two P3 test lines that were not run

The roadmap's P3 test line asks for two things this record does not contain, and
neither was estimated:

- **An idle 5 min VRAM sample: not measured.** The idle sample here is the one
  taken after the 5 s idle grace elapsed inside a 2-session run
  (`src/ResidentWorkerLoop.h:46`). A helper that idles for 5 minutes has hit the
  30 s idle exit budget and is gone four and a half minutes earlier, so a
  5-minute sample is a different measurement of a different thing and needs its
  own instrument.
- **An 8-12 GB card idle + other apps smoke: not measured.** This machine has a
  16 GB card (16047 MiB). The case B exists for - 361 MiB deciding whether a
  second application fits - is precisely the case this machine cannot produce.
  Whether B's 361 MiB is the difference on a 8-12 GB card is inferred from the
  arithmetic, not measured here.

One further branch is unexercised: `observed=not-freed`, the recorded outcome
when DLSS declines to hand the feature workset back. It exists because the
runtime is documented to keep feature memory, and on driver 32.0.16.1047 it did
not decline. No claim is made about a runtime that does.
