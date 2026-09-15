# P3's third session pair: what to run, and why nobody here can

`REPORT.md` beside this file carries the idle-VRAM trade at **n=2 per arm**, one short
of this project's own stated minimum ("one sample is not a measurement; the default of
three is the minimum this project accepts" - `player_session.ps1`'s own
documentation). The third pair was attempted twice and refused both times for the same
reason, and it is not a code problem.

## Why it cannot be run unattended

`player_session.ps1` drives the real player and injects the neural toggle chord with
`SendInput`. A locked Windows desktop refuses injected input: the secure desktop is
not the one the player's window lives on. The harness now reports that condition
deterministically instead of inferring it from a flapping foreground window - the fix
that shipped 2026-09-14:

```
SendInput refused the chord, Win32 error 5; the foreground window is none - the
session manager reports this session locked
(WTSSessionInfoEx SessionFlags = WTS_SESSIONSTATE_LOCK)
```

Checked again on 2026-09-15 before writing this file, with the same probe the harness
uses (`WTSQuerySessionInformationW`, `WTSSessionInfoEx`, level 1):

```
level=1 sessionId=1 state=0 flags=0 -> LOCKED
```

The read is valid (level 1 carries `SessionFlags`), so this is the session manager's
own record, not a guess. **An interactive, unlocked desktop is the missing
instrument** - there is nothing to fix and nothing to build.

## The two commands

Run them at the console, signed in, screen unlocked, with nothing else contending for
the GPU. `-Sessions 3` is deliberate: three per arm is the minimum, and the existing
two pairs are not reusable as part of a three-sample spread taken on a different day.

Set only the one key. `DLSSVideoPlayer.ini` is the player's whole profile - the eight
`NR*` art keys, the measured render pace, the upscaling state - and `-FreshProfile
Never` exists to preserve it, so overwriting the file to select a policy would defeat
the flag being passed and hand both arms a cold profile. Write the key in place:

```powershell
function Set-IniKey([string]$path, [string]$section, [string]$key, [string]$value) {
  # Win32 writes the section and key if absent and leaves every other key alone,
  # which is the same API the player reads them back with. The guard matters: this
  # function is called once per arm in the same session, and a second Add-Type of
  # the same type throws.
  if (-not ('W.P' -as [type])) {
    Add-Type -Namespace W -Name P -MemberDefinition '
      [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
      public static extern bool WritePrivateProfileStringW(string s, string k, string v, string f);'
  }
  if (-not [W.P]::WritePrivateProfileStringW($section, $key, $value, (Resolve-Path $path))) {
    throw "WritePrivateProfileString failed: $([ComponentModel.Win32Exception]::new())"
  }
}
```

```powershell
# One invocation per arm, three sessions each, which keeps the harness's own spread
# summary. Every session's helper log is saved by the harness itself as
# <OutJson>.sessionN.helper.log - added 2026-09-15 for exactly this measurement,
# because `src/Log.h:33` truncates the helper's log on every helper start and the
# existing per-session copy covered only the player's. The harness refuses to copy a
# helper log whose write time did not move, so a session that never started a helper
# leaves no file instead of inheriting the previous session's lines.
foreach ($arm in 'keep', 'free') {
  Set-IniKey build-upscaling\Release\DLSSVideoPlayer.ini NeuralHelper IdleVramPolicy $arm
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\verification\player_session.ps1 `
    -Player build-upscaling\Release\DLSSVideoPlayer.exe `
    -Media <a clip of 90 s or more; the same clip for both arms> `
    -SecondToggle -SecondToggleSeekBacks 7 `
    -FreshProfile Never -DropRenderCache -Sessions 3 `
    -OutJson $env:TEMP\p3-$arm.json -Note "P3 idle VRAM policy $arm"
}
```

`-FreshProfile Never` is load-bearing and has its own section in `REPORT.md`: any
other value makes `player_session.ps1:785` delete `DLSSVideoPlayer.ini`, which is the
file that selects the policy. That is exactly how the first attempt produced two arms
that were secretly one, both silently running `kDefaultIdleVramPolicy` (`keep`).

## The per-session assertion - check this before believing any number

Every session must be shown to *be* the arm it is labelled, from the helper's own log
rather than from the label. Six copies, six answers - not one per arm:

```powershell
# The harness's own per-session helper copies, never the live file: that holds only
# the last helper process's lines, whichever session wrote them last.
foreach ($f in Get-ChildItem $env:TEMP\p3-*.session*.helper.log | Sort-Object Name) {
  $hit = Select-String -Path $f -Pattern 'idleVramPolicy=' | Select-Object -First 1
  "{0,-34} {1}" -f $f.Name, ($hit.Line -replace '.*(idleVramPolicy=\w+).*', '$1')
}
# Expect six lines: p3-free.session1..3 all free, p3-keep.session1..3 all keep.
# Fewer than six means a session started no helper - not a sample. The JSON says
# which case each session was, so this is not inferred from a missing file:
#   copied       - this session's helper wrote its log and it was saved
#   staleRefused - the log was there but untouched, so it is the previous
#                  session's and was deliberately NOT copied
#   absent       - no helper log exists at all
#   empty        - the log moved but read back empty
#   copyFailed   - the log was there and moved, but reading or writing it threw
#   notRequested - no -OutJson, so no copy was ever going to be made
foreach ($arm in 'keep', 'free') {
  (Get-Content $env:TEMP\p3-$arm.json -Raw | ConvertFrom-Json).sessions |
    Select-Object index, @{n='outcome'; e={ $_.helperLog.outcome }}, helperLogCopy
}
Select-String -Path $env:TEMP\p3-*.session*.helper.log -Pattern 'post-job VRAM'
```

Why that file. `src/Log.h:28-31` names the log after the **running module's**
directory, so `DLSSVideoPlayer.exe` writes
`build-upscaling\Release\DLSSVideoPlayer.log` - the one the harness has always
copied as `logCopy` - while `NeuralWorker.exe` lives in `neural-runtime\` and
writes its own file there. `idleVramPolicy=` is emitted by `NeuralWorkerMain`, so it
is only ever in the second one, and `Log.h:33` truncates it on every helper start -
which is why the harness now copies it per session as `helperLogCopy` rather than
leaving the operator to do it per arm and assert one sample of three.

The `keep` arm must print `idleVramPolicy=keep` and no `released=1`; the `free` arm
must print `idleVramPolicy=free` with `observed=freed` and a `freedMiB` figure. If a
run labelled `free` logs `keep`, the ini was deleted - discard the pair and start
again.

## What the third pair decides

The recorded trade is **361 MiB of 1061 held while idle** against **+0.70 s on every
reuse** (3.20-3.24 s vs 2.48-2.54 s, +28 %). The direction is not in doubt - the two
arms do not overlap - but the seven-tenths is two samples, so `REPORT.md` says to
quote the direction and not the figure. A third pair either turns that into a figure
with a spread, or it shows the spread is wide enough that even the direction needs
more. `keep` ships either way: reuse latency is P1's whole purpose, and `free` is one
ini key away for a card where 361 MiB decides whether a second application fits.

Owed as of 2026-09-15. Blocked on an unlocked desktop, not on this repository.
