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
  # which is the same API the player reads them back with.
  Add-Type -Namespace W -Name P -MemberDefinition '
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool WritePrivateProfileStringW(string s, string k, string v, string f);'
  if (-not [W.P]::WritePrivateProfileStringW($section, $key, $value, (Resolve-Path $path))) {
    throw "WritePrivateProfileString failed: $([ComponentModel.Win32Exception]::new())"
  }
}
```

```powershell
# keep arm - the shipped default
Set-IniKey build-upscaling\Release\DLSSVideoPlayer.ini NeuralHelper IdleVramPolicy keep
powershell -NoProfile -ExecutionPolicy Bypass -File tools\verification\player_session.ps1 `
  -Player build-upscaling\Release\DLSSVideoPlayer.exe `
  -Media <a clip of 90 s or more> `
  -SecondToggle -SecondToggleSeekBacks 7 `
  -FreshProfile Never -DropRenderCache -Sessions 3 `
  -OutJson $env:TEMP\p3-keep.json -Note 'P3 idle VRAM policy keep'

# free arm
Set-IniKey build-upscaling\Release\DLSSVideoPlayer.ini NeuralHelper IdleVramPolicy free
powershell -NoProfile -ExecutionPolicy Bypass -File tools\verification\player_session.ps1 `
  -Player build-upscaling\Release\DLSSVideoPlayer.exe `
  -Media <the same clip> `
  -SecondToggle -SecondToggleSeekBacks 7 `
  -FreshProfile Never -DropRenderCache -Sessions 3 `
  -OutJson $env:TEMP\p3-free.json -Note 'P3 idle VRAM policy free'
```

`-FreshProfile Never` is load-bearing and has its own section in `REPORT.md`: any
other value makes `player_session.ps1:785` delete `DLSSVideoPlayer.ini`, which is the
file that selects the policy. That is exactly how the first attempt produced two arms
that were secretly one, both silently running `kDefaultIdleVramPolicy` (`keep`).

## The per-arm assertion - check this before believing any number

Both arms must be shown to *be* the arm they are labelled, from the helper's own log
rather than from the label:

```powershell
# The helper's log, not the player's. `src/Log.h:28-31` names the log after the
# RUNNING MODULE's directory, so DLSSVideoPlayer.exe writes
# build-upscaling\Release\DLSSVideoPlayer.log (which is what player_session.ps1
# scrapes, and what -OutJson copies as `logCopy`), while NeuralWorker.exe lives in
# neural-runtime\ and writes its own file there. `idleVramPolicy=` is emitted by
# NeuralWorkerMain, so it is only ever in the second one.
Select-String -Path build-upscaling\Release\neural-runtime\DLSSVideoPlayer.log `
  -Pattern 'idleVramPolicy=', 'post-job VRAM'
```

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
