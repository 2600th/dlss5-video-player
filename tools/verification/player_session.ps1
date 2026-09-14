<#
.SYNOPSIS
Drives a real DLSSVideoPlayer session unattended and times the neural toggle.

.DESCRIPTION
Every player-side number in this repository is an estimate, because the
benchmark harness drives NeuralWorker.exe headlessly and never opens the
player. This script closes that gap: it launches the player on a clip, waits
for the window, injects the player's own neural-render accelerator through
SendInput, and reads DLSSVideoPlayer.log until the render reports back. What
comes out is the wall-clock number the persistent-helper item is judged on -
keypress to the first neural frame on screen - measured, not modelled.

The player's log is the instrument's only sensor, which is deliberate: the
lines it matches on are the ones the product already writes, so the script
measures the shipped behaviour instead of a test hook. Every pattern is quoted
beside the source that emits it, and every match is echoed back into the JSON
as a raw excerpt so a reader can check the scrape rather than trust it.

A missed pattern must never read as a fast session. Each failure mode exits
with its own code and an explicit reason, and the JSON always carries
`outcome.ok`.

.PARAMETER Player
Path to DLSSVideoPlayer.exe. Its directory must hold the staged
neural-runtime, ffmpeg.exe and ffprobe.exe (the build tree's Release folder).

.PARAMETER Media
Path to the clip to play. A local file; URLs are out of scope.

.PARAMETER ToggleAfterSeconds
Seconds to let the clip play before the toggle is injected. The toggle is
never sent before the player logs its render device, so this is a floor on the
playhead position, not a substitute for readiness.

.PARAMETER RunSeconds
Seconds to leave the session running after the first neural frame before the
toggle is sent again to stop it. The render publishes its cache entry and
receipt when it finishes, and stopping the session is what makes the player
report its measured pace, so this bounds both.

.PARAMETER Sessions
How many sessions to drive in one invocation. One sample is not a measurement;
the default of three is the minimum this project accepts, and the summary
block reports the spread.

.PARAMETER FreshProfile
Never  - keep whatever DLSSVideoPlayer.ini and cache/ already exist.
First  - clear them before the first session only: one cold session, the rest
         warm (the ini carries [NeuralPace], which shortens the start lead).
Each   - clear them before every session, so every session is cold.

.PARAMETER Accelerator
CtrlAltD - default. The global overlay hotkey the player registers with
           RegisterHotKey (src/main.cpp RegisterOverlayHotkeys,
           MOD_CONTROL|MOD_ALT|'D'), delivered by WM_HOTKEY regardless of
           focus. Reliable unattended.
D        - the documented user key (src/main.cpp WM_KEYDOWN, docs/USAGE.md).
           Needs the player window in the foreground; the script puts it there
           and verifies with GetForegroundWindow before injecting, and fails
           with code 11 when Windows declines the activation - which it does,
           two sessions in four on this machine.
Both run the same ToggleNeuralRendering().

.PARAMETER DropRenderCache
Removes cache/v1/renders and cache/v1/staging before each session, leaving the
preflight receipt and the ini in place. Needed for repeated measurement: a
live session whose render key is already cached publishes that entry without
rendering a segment, so it never attaches and has no toggle-to-picture number.

.PARAMETER Note
Free text stamped into the JSON, for the one fact the script cannot observe:
whether anything else was using the GPU.

.PARAMETER OutJson
Where to write the JSON, plus one copy of each session's log beside it. It is
printed on stdout either way.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools/verification/player_session.ps1 `
  -Player build-upscaling/Release/DLSSVideoPlayer.exe `
  -Media docs/media/neural-comparison-demo.mp4 `
  -Sessions 3 -OutJson session.json

.NOTES
Windows PowerShell 5.1, no external modules. Exit codes:
  0  every gate passed
  2  bad arguments, missing player or media
  3  the main window never appeared
  4  the player exited before the session finished
  5  the toggle was accepted by the window and refused by the player
  6  the toggle produced no response at all (the key never arrived)
  7  no first neural frame
  8  no measured render pace
  9  a modal dialog blocked the session
 10  a neural frame was presented but no cold-start line was logged
 11  the player window could not be brought to the foreground
 12  another instance of this player is already running
 13  Windows refused to inject the keystroke and the foreground window does not
     explain it
 14  Windows refused to inject the keystroke because the workstation is locked
     or a higher-integrity window holds the foreground. Not detectable before
     the fact: the modern lock screen is a protected window on the ordinary
     desktop, so OpenInputDesktop still succeeds and the first chord is even
     accepted - it simply goes nowhere.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Player,
    [Parameter(Mandatory = $true)][string]$Media,
    [double]$ToggleAfterSeconds = 4.0,
    [double]$RunSeconds = 30.0,
    [int]$Sessions = 3,
    [ValidateSet('Never', 'First', 'Each')][string]$FreshProfile = 'First',
    # CtrlAltD by default, and not because it is the nicer API. Plain D needs
    # the player in the foreground, and Windows refuses SetForegroundWindow to
    # a process that did not receive the last input: two of four sessions in the
    # first idle batch died on exactly that. The registered hotkey reaches the
    # same ToggleNeuralRendering() through WM_HOTKEY with no focus at all.
    [ValidateSet('D', 'CtrlAltD')][string]$Accelerator = 'CtrlAltD',
    # Removes cache/v1/renders before every session and leaves the preflight
    # receipt and the ini alone. A live session whose render key is already in
    # the cache publishes that entry without rendering a segment, so its
    # segment index stays empty, the attach never fires and no neural frame is
    # ever presented - the session has no toggle-to-picture number at all. This
    # is how a second measurement at the same playhead behaves, so a repeated
    # measurement needs the render entries gone and the warm preflight kept.
    [switch]$DropRenderCache,
    # Toggle a second time in the same player process and measure that too. This
    # is the only way to see a resident helper: residency lives in the player's
    # lifetime, so a first toggle always pays a cold bring-up. The gap lets
    # playback advance, which moves the snapped playhead and therefore the render
    # key - at the same playhead the toggle is answered from the cache and no
    # helper job runs at all.
    [switch]$SecondToggle,
    [double]$SecondToggleGapSeconds = 4.0,
    [string]$OutJson,
    [double]$WindowTimeoutSeconds = 45.0,
    [double]$ReadyTimeoutSeconds = 45.0,
    [double]$ToggleGraceSeconds = 8.0,
    [double]$NeuralFrameTimeoutSeconds = 90.0,
    [double]$PaceTimeoutSeconds = 30.0,
    [double]$ExitTimeoutSeconds = 30.0,
    [double]$SettleSeconds = 3.0,
    # Free text stamped into the JSON. Whether the GPU was contended is not
    # something the script can see, and a timing number without that fact is
    # not citable, so the operator states it and it travels with the numbers.
    [string]$Note
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- exit codes
$EXIT_OK = 0
$EXIT_ARGUMENTS = 2
$EXIT_NO_WINDOW = 3
$EXIT_PLAYER_EXITED = 4
$EXIT_TOGGLE_REFUSED = 5
$EXIT_TOGGLE_UNANSWERED = 6
$EXIT_NO_NEURAL_FRAME = 7
$EXIT_NO_PACE = 8
$EXIT_DIALOG = 9
$EXIT_NO_COLD_START = 10
$EXIT_NO_FOREGROUND = 11
$EXIT_PLAYER_RUNNING = 12
$EXIT_INJECTION_DENIED = 13
$EXIT_DESKTOP_LOCKED = 14

function Write-Progress-Line([string]$text) {
    # stderr, so stdout carries nothing but the JSON document.
    [Console]::Error.WriteLine(('[player_session] ' + $text))
}

# ------------------------------------------------------------------ win32
if (-not ('DlssPlayerSession.Win32' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace DlssPlayerSession {
    // INPUT's union is aligned by its widest member, so the union is declared
    // as a union and the runtime computes the offset. Hand-padding it gets the
    // keyboard fields right on x86 and four bytes wrong on x64.
    [StructLayout(LayoutKind.Sequential)]
    public struct MouseInput {
        public int dx; public int dy; public uint mouseData;
        public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct KeybdInput {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public MouseInput mi;
        [FieldOffset(0)] public KeybdInput ki;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct Input {
        public uint type;
        public InputUnion u;
    }

    public static class Win32 {
        const uint INPUT_KEYBOARD = 1;
        const uint KEYEVENTF_KEYUP = 0x0002;
        public const int WM_CLOSE = 0x0010;

        [DllImport("user32.dll", SetLastError = true)]
        static extern uint SendInput(uint nInputs, Input[] pInputs, int cbSize);
        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hWnd);
        [DllImport("user32.dll")]
        public static extern bool IsWindow(IntPtr hWnd);
        [DllImport("user32.dll")]
        public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")]
        public static extern bool SetForegroundWindow(IntPtr hWnd);
        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
        [DllImport("user32.dll")]
        public static extern bool BringWindowToTop(IntPtr hWnd);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern bool PostMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")]
        static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
        [DllImport("user32.dll")]
        static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int GetClassName(IntPtr hWnd, StringBuilder text, int count);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);

        // Why SendInput would be refused, or null when it would not. Called
        // only after a refusal, to explain it - never as a predicate.
        //
        // OpenInputDesktop is the textbook check and it is wrong here: the
        // modern lock screen is a protected window on the ordinary Default
        // desktop, so OpenInputDesktop still succeeds while injection is
        // refused. Observed directly - an up-front check passed, the first
        // chord was accepted and went nowhere, the second came back
        // ERROR_ACCESS_DENIED. That guard was deleted rather than shipped.
        //
        // What decides it is the foreground window: SendInput is refused when
        // that window's process outranks ours, and GetForegroundWindow returns
        // null while the workstation is locked. But it does not return null
        // *consistently* while locked - polling it eight times at 700 ms on a
        // locked machine returned null twice and the lock-screen window six
        // times, and one unlucky single sample is what made an earlier version
        // of this script report a locked box as unlocked. So the foreground is
        // sampled several times and any null decides it.
        public static string InjectionBlockedReason() {
            const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
            string outranked = null;
            for (int sample = 0; sample < 5; ++sample) {
                if (sample > 0) System.Threading.Thread.Sleep(120);
                IntPtr window = GetForegroundWindow();
                if (window == IntPtr.Zero)
                    return "no window held the foreground on sample " + (sample + 1) +
                           " of 5, which is what a locked workstation looks like from here";
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, owner);
                if (process == IntPtr.Zero) outranked = ForegroundWindowInfo();
                else CloseHandle(process);
            }
            if (outranked != null)
                return "the foreground window outranks this process (" + outranked +
                       "), so injected input is refused";
            return null;
        }

        // Whatever holds the foreground, named: when injection is refused this
        // is the one fact that says why.
        public static string ForegroundWindowInfo() {
            IntPtr window = GetForegroundWindow();
            if (window == IntPtr.Zero) return "none";
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            StringBuilder className = new StringBuilder(256);
            GetClassName(window, className, className.Capacity);
            StringBuilder title = new StringBuilder(512);
            GetWindowText(window, title, title.Capacity);
            return "pid=" + owner + " class=" + className.ToString() + " title=\"" + title.ToString() + "\"";
        }

        delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr param);

        // One SendInput call per chord: a batch is injected atomically, so no
        // other injected key can land between the modifier and the letter and
        // break the hotkey the player registered.
        public static int SendKeyChord(ushort[] modifiers, ushort key) {
            List<Input> inputs = new List<Input>();
            foreach (ushort modifier in modifiers) inputs.Add(Key(modifier, false));
            inputs.Add(Key(key, false));
            inputs.Add(Key(key, true));
            for (int i = modifiers.Length - 1; i >= 0; --i) inputs.Add(Key(modifiers[i], true));
            Input[] batch = inputs.ToArray();
            uint sent = SendInput((uint)batch.Length, batch, Marshal.SizeOf(typeof(Input)));
            if (sent != (uint)batch.Length) return -Marshal.GetLastWin32Error();
            return batch.Length;
        }

        static Input Key(ushort vk, bool up) {
            Input input = new Input();
            input.type = INPUT_KEYBOARD;
            input.u.ki.wVk = vk;
            input.u.ki.wScan = 0;
            input.u.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0u;
            input.u.ki.time = 0;
            input.u.ki.dwExtraInfo = IntPtr.Zero;
            return input;
        }

        // Top-level windows of one process, as "class|title". The session is
        // unattended, so a dialog is not a prompt, it is a stall: the caller
        // looks for the common dialog class (#32770) and fails loudly.
        public static string[] ProcessWindows(int processId) {
            List<string> found = new List<string>();
            EnumWindows(delegate(IntPtr hWnd, IntPtr param) {
                uint owner;
                GetWindowThreadProcessId(hWnd, out owner);
                if (owner != (uint)processId) return true;
                if (!IsWindowVisible(hWnd)) return true;
                StringBuilder className = new StringBuilder(256);
                GetClassName(hWnd, className, className.Capacity);
                StringBuilder title = new StringBuilder(512);
                GetWindowText(hWnd, title, title.Capacity);
                found.Add(className.ToString() + "|" + title.ToString());
                return true;
            }, IntPtr.Zero);
            return found.ToArray();
        }
    }
}
'@
}

$VK = @{ Control = 0x11; Menu = 0x12; D = 0x44 }
$SW_RESTORE = 9
$DIALOG_CLASS = '#32770'

# ------------------------------------------------------------------ log I/O
# The player holds the log open for writing and flushes every line, so the
# share mode has to allow it. FileShare.Read would fail against the writer.
function Read-LogText([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return '' }
    $share = [System.IO.FileShare]'ReadWrite, Delete'
    try {
        $stream = New-Object System.IO.FileStream($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, $share)
    } catch [System.IO.IOException] {
        return ''
    }
    try {
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8, $true)
        return $reader.ReadToEnd()
    } finally {
        $stream.Dispose()
    }
}

# src/Log.h: "[HH:MM:SS.mmm] text". No date, so the launch date anchors it and
# a backwards step of more than an hour is midnight, not a clock going
# backwards. Both this clock and Get-Date come from the system time, so the
# difference of two of them is exact to that clock's granularity (~15.6 ms
# unless something raised the timer resolution) with no cross-clock skew.
function Parse-LogLines([string]$text, [datetime]$anchor, [int]$skipLines) {
    $entries = New-Object System.Collections.ArrayList
    if ([string]::IsNullOrEmpty($text)) { return $entries }
    $lines = $text -split "`r?`n"
    $day = $anchor.Date
    $previous = $null
    $index = -1
    foreach ($line in $lines) {
        $index++
        if ($index -lt $skipLines) { continue }
        if ($line -notmatch '^\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\] (.*)$') { continue }
        $timeOfDay = New-TimeSpan -Hours ([int]$Matches[1]) -Minutes ([int]$Matches[2]) -Seconds ([int]$Matches[3])
        $timeOfDay = $timeOfDay.Add([TimeSpan]::FromMilliseconds([int]$Matches[4]))
        $stamp = $day.Add($timeOfDay)
        if ($previous -ne $null -and $stamp -lt $previous.AddHours(-1)) {
            $day = $day.AddDays(1)
            $stamp = $stamp.AddDays(1)
        }
        $previous = $stamp
        [void]$entries.Add([pscustomobject]@{ Time = $stamp; Text = $Matches[5]; Raw = $line })
    }
    return $entries
}

function Count-Lines([string]$text) {
    if ([string]::IsNullOrEmpty($text)) { return 0 }
    return ($text -split "`r?`n").Count
}

# ------------------------------------------------------------------ watching
# One wait primitive for the whole script: poll the log for the first line
# matching any wanted pattern, and give up the moment something that rules the
# session out appears - a refusal line, a modal dialog, a dead player.
#
# `Since` is not a convenience. A player that opens a clip already runs one
# cache-check job, and that job logs its own "Neural cold start:" line before
# the toggle is ever pressed. Matching it would have reported a session that
# presented a neural frame 3 s before the key went in, which is exactly the
# silent-success failure this instrument exists to make impossible. Every wait
# after the toggle is therefore windowed to the toggle instant.
function Wait-ForLogLine {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Context,
        [Parameter(Mandatory = $true)][string[]]$Wanted,
        [string[]]$Refusals = @(),
        [Parameter(Mandatory = $true)][double]$TimeoutSeconds,
        [datetime]$Since = [datetime]::MinValue,
        [switch]$IgnoreExit
    )
    # The log's stamp and Get-Date both read the system clock, whose tick is
    # ~15.6 ms, and the player can handle the key before SendInput returns. A
    # 20 ms slack keeps that quantisation from hiding the line it caused.
    $floor = $Since
    if ($Since -ne [datetime]::MinValue) { $floor = $Since.AddMilliseconds(-20) }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ($true) {
        $text = Read-LogText $Context.LogPath
        $entries = Parse-LogLines $text $Context.Anchor $Context.SkipLines
        foreach ($entry in $entries) {
            if ($entry.Time -lt $floor) { continue }
            foreach ($pattern in $Wanted) {
                if ($entry.Text -match $pattern) {
                    return [pscustomobject]@{ Status = 'Matched'; Entry = $entry; Pattern = $pattern }
                }
            }
            foreach ($pattern in $Refusals) {
                if ($entry.Text -match $pattern) {
                    return [pscustomobject]@{ Status = 'Refused'; Entry = $entry; Pattern = $pattern }
                }
            }
        }
        $dialogs = @([DlssPlayerSession.Win32]::ProcessWindows($Context.Process.Id) | Where-Object { $_ -like ($DIALOG_CLASS + '|*') })
        if ($dialogs.Count -gt 0) {
            return [pscustomobject]@{ Status = 'Dialog'; Entry = $null; Pattern = ($dialogs -join ' ; ') }
        }
        if (-not $IgnoreExit) {
            $Context.Process.Refresh()
            if ($Context.Process.HasExited) {
                return [pscustomobject]@{ Status = 'Exited'; Entry = $null; Pattern = ('exit code ' + $Context.Process.ExitCode) }
            }
        }
        if ((Get-Date) -ge $deadline) {
            return [pscustomobject]@{ Status = 'Timeout'; Entry = $null; Pattern = $null }
        }
        Start-Sleep -Milliseconds 100
    }
}

# Last line matching a pattern, optionally only from `Since` onward.
function Find-LogLine {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Context,
        [Parameter(Mandatory = $true)][string]$Pattern,
        [datetime]$Since = [datetime]::MinValue
    )
    $floor = $Since
    if ($Since -ne [datetime]::MinValue) { $floor = $Since.AddMilliseconds(-20) }
    $entries = Parse-LogLines (Read-LogText $Context.LogPath) $Context.Anchor $Context.SkipLines
    $hit = $null
    foreach ($entry in $entries) {
        if ($entry.Time -lt $floor) { continue }
        if ($entry.Text -match $Pattern) { $hit = $entry }
    }
    return $hit
}

function Focus-Player([hashtable]$Context) {
    $handle = $Context.Window
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
        if ([DlssPlayerSession.Win32]::GetForegroundWindow() -eq $handle) { return $true }
        [void][DlssPlayerSession.Win32]::ShowWindow($handle, $SW_RESTORE)
        [void][DlssPlayerSession.Win32]::BringWindowToTop($handle)
        [void][DlssPlayerSession.Win32]::SetForegroundWindow($handle)
        Start-Sleep -Milliseconds 150
    }
    return ([DlssPlayerSession.Win32]::GetForegroundWindow() -eq $handle)
}

# Injects the neural toggle and returns the instant it went in. Plain D is a
# WM_KEYDOWN the focused window has to receive; Ctrl+Alt+D is a registered
# hotkey the window gets wherever focus is.
function Send-NeuralToggle([hashtable]$Context) {
    if ($Context.Accelerator -eq 'D') {
        if (-not (Focus-Player $Context)) {
            return [pscustomobject]@{ Ok = $false; Reason = 'the player window would not take the foreground'; Code = $EXIT_NO_FOREGROUND }
        }
        $modifiers = @()
    } else {
        $modifiers = @([uint16]$VK.Control, [uint16]$VK.Menu)
    }
    $sentAt = Get-Date
    $sent = [DlssPlayerSession.Win32]::SendKeyChord([uint16[]]$modifiers, [uint16]$VK.D)
    $doneAt = Get-Date
    if ($sent -le 0) {
        # ERROR_ACCESS_DENIED here is not the player ignoring a key: the key was
        # never injected. Conflating the two would put the blame on the player.
        $reason = 'SendInput refused the chord, Win32 error ' + (-$sent) +
                  '; the foreground window is ' + [DlssPlayerSession.Win32]::ForegroundWindowInfo()
        $blocked = [DlssPlayerSession.Win32]::InjectionBlockedReason()
        if ($blocked) {
            return [pscustomobject]@{ Ok = $false; Reason = ($reason + ' - ' + $blocked); Code = $EXIT_DESKTOP_LOCKED }
        }
        return [pscustomobject]@{ Ok = $false; Reason = $reason; Code = $EXIT_INJECTION_DENIED }
    }
    return [pscustomobject]@{ Ok = $true; At = $sentAt; Done = $doneAt; Events = $sent; Reason = $null }
}

# ------------------------------------------------------------------ patterns
# Each pattern is quoted beside the source line that writes it. Changing a log
# line in the player must change this table, which is why they are together.
$P = @{
    # src/main.cpp:793 "Isolated neural helper available; player remains hook-free. GPU=..."
    Startup       = 'Isolated neural helper available.*GPU=(?<gpu>.*) driver=(?<driver>\S*) generation=(?<generation>\S+)'
    # src/D3D12Renderer.cpp:83 - the player's own render device, so the clip is open and the renderer is up.
    Device        = 'D3D12 device adapter "(?<adapter>[^"]*)" luid=(?<luid>\S+)'
    # src/main.cpp:3006 and :3004 - the toggle was accepted and a session exists.
    SessionStart  = '^Active neural session (started at (?<from>[0-9.eE+-]+) s through (?<to>[0-9.eE+-]+) s|replaying )'
    # src/main.cpp:2926 - a session with no pace prior for this GPU says so first.
    PaceUnknown   = '^Active neural session pace is unmeasured on this GPU'
    # src/main.cpp:4183, :4179, :2960, :2983 - every way the toggle can be declined.
    Refusals      = @(
        '^Neural rendering toggle ignored:',
        '^Neural rendering toggle deferred until the seek lands\.',
        '^Active neural session refused:',
        '^Active neural session could not create its segment directory\.'
    )
    # src/main.cpp:3380 via NoteNeuralFramePresented - the first neural frame is
    # on screen. `total=` is required to be a number, not a dash: the total is
    # only recorded by NeuralColdStartRecord::Presented, so a dash is a job that
    # ended without ever putting a neural frame on screen - which is what the
    # cache-check job at open logs, and what must never be read as a picture.
    ColdStart     = '^Neural cold start: total=(?<total>[0-9.]+)s (?<fields>.+)$'
    ColdStartAny  = '^Neural cold start: (?<fields>.+)$'
    # src/main.cpp:3097 - the same event, one line later, with the buffered lead.
    Attach        = '^Active neural playback attached at (?<at>[0-9.eE+-]+) s with (?<lead>[0-9.eE+-]+) s buffered\.'
    # src/main.cpp:3091, :3079, :3080, :3086 - presentation failures.
    AttachFailed  = @(
        '^Active neural playback could not '
    )
    # src/main.cpp:3061, :3406 - the render gave up or was cancelled.
    SessionFailed = @(
        '^Active neural session ended early:'
    )
    # src/main.cpp:2735 - a hotkey another process already owns is not
    # registered, so Ctrl+Alt+D would go nowhere and the session would read as
    # "the key did not arrive" without saying why.
    HotkeyTaken   = '^Overlay hotkey unavailable: Ctrl\+Alt\+D winerr=(?<error>\d+)'
    # src/main.cpp:3619 - the receipt summary, written when receipt.json is.
    Receipt       = '^Neural render receipt: (?<summary>.+)$'
    # src/main.cpp:3058 - the published cache entry; receipt.json is its sibling.
    CacheEntry    = '^Active neural session rendered (?<frames>\d+) frames and published its cache entry; save=(?<save>\d+) entry=(?<entry>.+)$'
    # src/main.cpp:1524 via RecordLiveRenderPace, needs >= 120 rendered frames.
    Pace          = '^Measured neural render pace: (?<width>\d+)x(?<height>\d+) at (?<ms>[0-9.eE+-]+) ms/frame over (?<frames>\d+) frames \((?<scale>[0-9.eE+-]+)x the reference GPU\); (?<geometries>\d+) geometries known'
    # src/main.cpp:3046 - the session is down. Logged after the pace line, so
    # seeing this without a pace line is proof the pace was never measured.
    Stopped       = '^Active neural session stopped at (?<at>[0-9.eE+-]+) s; presented=(?<presented>\d+) dropped=(?<dropped>\d+)'
    # src/main.cpp:1008 - where the cache entry and its receipt live.
    CacheDir      = '^Neural cache directory: (?<root>.+)$'
    # src/main.cpp:3559 - present only when the live job actually rendered. Its
    # absence after a session start means the render key was already cached.
    RenderStart   = '^Neural cache miss or invalid entry; starting a new render\.'
    # src/NeuralReceipt.cpp Seconds(): a dash for the total means
    # NeuralColdStartRecord::Presented never ran, so the job ended without ever
    # putting a neural frame on screen. Distinguishing this from a slow session
    # is the difference between a diagnosis and a 90 s timeout.
    NoPicture     = '^Neural cold start: total=-'
}

function Parse-ColdStartFields([string]$fields) {
    # src/NeuralReceipt.cpp SummarizeNeuralColdStartForLog: "key=1.234s" or "key=-".
    $phases = [ordered]@{}
    foreach ($match in [regex]::Matches($fields, '(?<key>[A-Za-z]+)=(?:(?<seconds>[0-9.]+)s|-)')) {
        $key = $match.Groups['key'].Value
        if ($match.Groups['seconds'].Success) {
            $phases[$key] = [double]$match.Groups['seconds'].Value
        } else {
            # A phase that did not happen is null, never 0: NeuralColdStartTimeline
            # keeps "did not run" and "took no time" apart and so does this.
            $phases[$key] = $null
        }
    }
    return $phases
}

function Seconds-Between([datetime]$from, [datetime]$to) {
    return [Math]::Round(($to - $from).TotalSeconds, 3)
}

function Summarize-Numbers([double[]]$values) {
    if ($values.Count -eq 0) { return $null }
    $sorted = @($values | Sort-Object)
    $count = $sorted.Count
    if ($count % 2 -eq 1) {
        $median = $sorted[[int](($count - 1) / 2)]
    } else {
        $median = ($sorted[$count / 2 - 1] + $sorted[$count / 2]) / 2.0
    }
    $sum = 0.0
    foreach ($value in $sorted) { $sum += $value }
    return [ordered]@{
        count  = $count
        min    = [Math]::Round($sorted[0], 3)
        median = [Math]::Round($median, 3)
        max    = [Math]::Round($sorted[$count - 1], 3)
        mean   = [Math]::Round($sum / $count, 3)
        spread = [Math]::Round($sorted[$count - 1] - $sorted[0], 3)
        values = @($sorted | ForEach-Object { [Math]::Round($_, 3) })
    }
}

function Stop-Player([hashtable]$Context) {
    $process = $Context.Process
    $process.Refresh()
    if ($process.HasExited) { return [pscustomobject]@{ Clean = $true; ExitCode = $process.ExitCode } }
    # WM_CLOSE is the shutdown the player is built for: it tears the session
    # down, releases the live segments and writes the ini back. A failure that
    # happened before a window existed has nothing to post to, and killing that
    # is not an unclean exit of a session - there was no session.
    $closed = $false
    if ($Context.Window -ne [IntPtr]::Zero -and [DlssPlayerSession.Win32]::IsWindow($Context.Window)) {
        [void][DlssPlayerSession.Win32]::PostMessage($Context.Window, [DlssPlayerSession.Win32]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        $closed = $true
        if ($process.WaitForExit([int]($ExitTimeoutSeconds * 1000))) {
            return [pscustomobject]@{ Clean = $true; ExitCode = $process.ExitCode }
        }
    }
    if ($closed) { Write-Progress-Line 'the player ignored WM_CLOSE; killing it' }
    else { Write-Progress-Line 'no window to close; killing the player' }
    try { $process.Kill() } catch { }
    [void]$process.WaitForExit(10000)
    return [pscustomobject]@{ Clean = $false; ExitCode = $null }
}

# ---------------------------------------------------------------- one session
function Invoke-PlayerSession {
    param(
        [Parameter(Mandatory = $true)][int]$Index,
        [Parameter(Mandatory = $true)][string]$PlayerPath,
        [Parameter(Mandatory = $true)][string]$MediaPath,
        [Parameter(Mandatory = $true)][bool]$Clear
    )

    $playerDirectory = Split-Path -Parent $PlayerPath
    $logPath = Join-Path $playerDirectory 'DLSSVideoPlayer.log'
    $iniPath = Join-Path $playerDirectory 'DLSSVideoPlayer.ini'
    $cachePath = Join-Path $playerDirectory 'cache'

    $record = [ordered]@{
        index    = $Index
        cold     = $Clear
        failure  = $null
        exitCode = $EXIT_OK
    }

    # A second instance would write the same log and the same ini, and the
    # record would be a blend of two sessions.
    $strays = @()
    foreach ($candidate in (Get-Process -Name 'DLSSVideoPlayer' -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $candidate.Path } catch { $path = '<unreadable>' }
        $strays += ('pid ' + $candidate.Id + ' ' + $path)
    }
    if ($strays.Count -gt 0) {
        $record.failure = 'another DLSSVideoPlayer is already running: ' + ($strays -join ', ')
        $record.exitCode = $EXIT_PLAYER_RUNNING
        return $record
    }

    $removed = @()
    if ($Clear) {
        if (Test-Path -LiteralPath $iniPath) { Remove-Item -LiteralPath $iniPath -Force; $removed += 'DLSSVideoPlayer.ini' }
        if (Test-Path -LiteralPath $cachePath) { Remove-Item -LiteralPath $cachePath -Recurse -Force; $removed += 'cache/' }
    } elseif ($DropRenderCache) {
        foreach ($leaf in @('renders', 'staging')) {
            $directory = Join-Path (Join-Path $cachePath 'v1') $leaf
            if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force; $removed += ('cache/v1/' + $leaf) }
        }
    }
    $record.cleared = $removed

    # The player truncates its log on the first LOG(), so clearing it is belt
    # and braces; when it cannot be removed the pre-existing line count is
    # noted instead and every parse skips past it.
    $logBytesBefore = 0
    $skipLines = 0
    $logCleared = $false
    if (Test-Path -LiteralPath $logPath) {
        $logBytesBefore = (Get-Item -LiteralPath $logPath).Length
        try {
            Remove-Item -LiteralPath $logPath -Force
            $logCleared = $true
        } catch {
            $skipLines = Count-Lines (Read-LogText $logPath)
        }
    } else {
        $logCleared = $true
    }
    $record.log = [ordered]@{
        path           = $logPath
        bytesBefore    = $logBytesBefore
        cleared        = $logCleared
        skippedLines   = $skipLines
    }

    $launchAt = Get-Date
    $process = Start-Process -FilePath $PlayerPath -ArgumentList @('"' + $MediaPath + '"') -PassThru
    $context = @{
        Process     = $process
        LogPath     = $logPath
        Anchor      = $launchAt
        SkipLines   = $skipLines
        Window      = [IntPtr]::Zero
        Accelerator = $Accelerator
    }

    try {
        # ---- window appear
        $windowAt = $null
        $windowDeadline = $launchAt.AddSeconds($WindowTimeoutSeconds)
        while ($true) {
            $process.Refresh()
            if ($process.HasExited) {
                $record.failure = 'the player exited before its window appeared, exit code ' + $process.ExitCode
                $record.exitCode = $EXIT_PLAYER_EXITED
                return $record
            }
            $handle = $process.MainWindowHandle
            if ($handle -ne [IntPtr]::Zero -and [DlssPlayerSession.Win32]::IsWindowVisible($handle)) {
                $windowAt = Get-Date
                $context.Window = $handle
                break
            }
            if ((Get-Date) -ge $windowDeadline) {
                $record.failure = 'no visible main window within ' + $WindowTimeoutSeconds + ' s'
                $record.exitCode = $EXIT_NO_WINDOW
                return $record
            }
            Start-Sleep -Milliseconds 25
        }
        Write-Progress-Line ('session ' + $Index + ': window after ' + (Seconds-Between $launchAt $windowAt) + ' s')

        # ---- ready to toggle: the clip is open and the render device is up
        $ready = Wait-ForLogLine -Context $context -Wanted @($P.Device) -TimeoutSeconds $ReadyTimeoutSeconds
        if ($ready.Status -ne 'Matched') {
            $record.failure = 'the player never logged a render device (' + $ready.Status + ' ' + $ready.Pattern + ')'
            if ($ready.Status -eq 'Dialog') { $record.exitCode = $EXIT_DIALOG }
            elseif ($ready.Status -eq 'Exited') { $record.exitCode = $EXIT_PLAYER_EXITED }
            else { $record.exitCode = $EXIT_NO_WINDOW }
            return $record
        }
        $readyAt = $ready.Entry.Time
        if ($Accelerator -eq 'CtrlAltD') {
            $taken = Find-LogLine -Context $context -Pattern $P.HotkeyTaken
            if ($taken -ne $null) {
                $record.failure = 'another process owns Ctrl+Alt+D, so the player never registered it: ' + $taken.Text
                $record.exitCode = $EXIT_NO_FOREGROUND
                return $record
            }
        }

        # The toggle floor is measured from the window, because that is the
        # instant a user could first have pressed the key.
        $toggleFloor = $windowAt.AddSeconds($ToggleAfterSeconds)
        while ((Get-Date) -lt $toggleFloor) { Start-Sleep -Milliseconds 50 }

        # ---- the toggle
        # One retry, and only into silence. A press that arrived is visible in
        # the log within the same message-loop turn - StartLiveNeuralSession and
        # every refusal log synchronously from the key handler - so a grace
        # window with neither a session line nor a refusal line means nothing
        # ran and a second press cannot toggle anything off. Pressing again
        # after a refusal, or after a session started, would corrupt the run,
        # so neither is retried. One session in fourteen needed this on
        # Ctrl+Alt+D, cause undetermined; the attempt count travels with the
        # numbers so a retried session is never silently equal to a clean one.
        $attemptLimit = 2
        $toggle = $null
        $accepted = $null
        $toggleAt = $null
        $attempts = 0
        while ($attempts -lt $attemptLimit) {
            $attempts++
            $toggle = Send-NeuralToggle $context
            if (-not $toggle.Ok) {
                $record.failure = 'the neural toggle could not be injected: ' + $toggle.Reason
                $record.exitCode = $toggle.Code
                $record.toggleAttempts = $attempts
                return $record
            }
            $toggleAt = $toggle.At
            Write-Progress-Line ('session ' + $Index + ': toggle injected (' + $Accelerator + ', attempt ' + $attempts + ')')
            $accepted = Wait-ForLogLine -Context $context -Wanted @($P.SessionStart) -Refusals $P.Refusals -TimeoutSeconds $ToggleGraceSeconds -Since $toggleAt
            if ($accepted.Status -ne 'Timeout') { break }
            if ($attempts -lt $attemptLimit) {
                Write-Progress-Line ('session ' + $Index + ': no response to the toggle; pressing once more')
            }
        }
        $record.toggleAttempts = $attempts
        if ($accepted.Status -eq 'Refused') {
            $record.failure = 'the player refused the toggle: ' + $accepted.Entry.Text
            $record.exitCode = $EXIT_TOGGLE_REFUSED
            $record.excerpts = [ordered]@{ refusal = $accepted.Entry.Raw }
            return $record
        }
        if ($accepted.Status -ne 'Matched') {
            # No session line and no refusal line: nothing in the player ran,
            # so the keystroke never reached the window. This is the one
            # outcome that must never be reported as a slow session.
            $record.failure = 'the toggle produced no log response at all after ' + $attempts + ' attempts (' + $accepted.Status + '); the key did not reach the window'
            if ($accepted.Status -eq 'Dialog') { $record.exitCode = $EXIT_DIALOG; $record.failure = 'a modal dialog blocked the session: ' + $accepted.Pattern }
            elseif ($accepted.Status -eq 'Exited') { $record.exitCode = $EXIT_PLAYER_EXITED }
            else { $record.exitCode = $EXIT_TOGGLE_UNANSWERED }
            return $record
        }

        # ---- first neural frame on screen
        $refusals = @()
        $refusals += $P.SessionFailed
        $refusals += $P.AttachFailed
        $refusals += $P.NoPicture
        $cold = Wait-ForLogLine -Context $context -Wanted @($P.ColdStart) -Refusals $refusals -TimeoutSeconds $NeuralFrameTimeoutSeconds -Since $toggleAt
        if ($cold.Status -ne 'Matched') {
            $reason = $cold.Status
            if ($cold.Entry -ne $null) { $reason = $cold.Entry.Text }
            if ($cold.Pattern -eq $P.NoPicture) {
                $reason = 'the neural job ended without presenting a frame'
                if ((Find-LogLine -Context $context -Pattern $P.RenderStart -Since $toggleAt) -eq $null) {
                    $reason += ': the render key was already in the cache, so the session published the existing entry, rendered no segment and never attached'
                }
                $reason += ' (' + $cold.Entry.Text + ')'
            }
            $record.failure = 'no first neural frame: ' + $reason
            if ($cold.Status -eq 'Dialog') { $record.exitCode = $EXIT_DIALOG }
            elseif ($cold.Status -eq 'Exited') { $record.exitCode = $EXIT_PLAYER_EXITED }
            else { $record.exitCode = $EXIT_NO_NEURAL_FRAME }
            return $record
        }
        $firstNeuralAt = $cold.Entry.Time
        $attach = Wait-ForLogLine -Context $context -Wanted @($P.Attach) -TimeoutSeconds 10.0 -Since $toggleAt
        if ($attach.Status -ne 'Matched') {
            # The cold-start line is only written from NoteNeuralFramePresented,
            # which the attach calls, so its absence here means the log changed
            # shape under the script rather than a slow session.
            $record.failure = 'a cold-start line was logged but no attach line followed it'
            $record.exitCode = $EXIT_NO_COLD_START
            return $record
        }
        Write-Progress-Line ('session ' + $Index + ': first neural frame ' + (Seconds-Between $toggleAt $firstNeuralAt) + ' s after the toggle')

        # ---- let the render finish so it writes its receipt, then stop it
        $completion = Wait-ForLogLine -Context $context -Wanted @($P.CacheEntry) -Refusals $P.SessionFailed -TimeoutSeconds $RunSeconds -Since $toggleAt
        $completionStatus = $completion.Status
        # A session with nothing more to render still has to be stopped for the
        # player to report its pace, so a timeout here is not fatal.
        Start-Sleep -Milliseconds ([int]($SettleSeconds * 1000))

        $toggleOff = Send-NeuralToggle $context
        if (-not $toggleOff.Ok) {
            $record.failure = 'the stop toggle could not be injected: ' + $toggleOff.Reason
            $record.exitCode = $toggleOff.Code
            return $record
        }
        $toggleOffAt = $toggleOff.At
        # Windowed to the toggle, not the stop toggle: playback reaching the end
        # of the clip releases the session by itself, and that release measures
        # the same pace a moment earlier.
        $pace = Wait-ForLogLine -Context $context -Wanted @($P.Pace) -Refusals @($P.Stopped) -TimeoutSeconds $PaceTimeoutSeconds -Since $toggleAt
        if ($pace.Status -eq 'Refused') {
            $record.failure = 'the session stopped without measuring a pace (fewer than 120 rendered frames): ' + $pace.Entry.Text
            $record.exitCode = $EXIT_NO_PACE
            return $record
        }
        if ($pace.Status -ne 'Matched') {
            $record.failure = 'no measured render pace (' + $pace.Status + ')'
            if ($pace.Status -eq 'Dialog') { $record.exitCode = $EXIT_DIALOG }
            elseif ($pace.Status -eq 'Exited') { $record.exitCode = $EXIT_PLAYER_EXITED }
            else { $record.exitCode = $EXIT_NO_PACE }
            return $record
        }
        [void](Wait-ForLogLine -Context $context -Wanted @($P.Stopped) -TimeoutSeconds 10.0 -Since $toggleAt)
        Write-Progress-Line ('session ' + $Index + ': pace ' + $pace.Entry.Text)

        # ---- optional: a second toggle in the SAME player process, which is the
        # only way to exercise a resident helper. Residency lives in the player's
        # own lifetime, so every first toggle in a process pays a cold bring-up
        # however well the helper is kept. Playback has continued while neural was
        # off, so this toggle snaps to a later playhead and therefore a different
        # render key - without that it would land on the entry the first session
        # just published and be answered from the cache with no helper job at all.
        $second = $null
        if ($SecondToggle) {
            Start-Sleep -Milliseconds ([int]($SecondToggleGapSeconds * 1000))
            $again = Send-NeuralToggle $context
            if (-not $again.Ok) {
                $record.failure = 'the second toggle could not be injected: ' + $again.Reason
                $record.exitCode = $again.Code
                return $record
            }
            $againAt = $again.At
            $secondCold = Wait-ForLogLine -Context $context -Wanted @($P.ColdStart) -Refusals $refusals -TimeoutSeconds $NeuralFrameTimeoutSeconds -Since $againAt
            if ($secondCold.Status -ne 'Matched') {
                $record.failure = 'no neural frame after the second toggle (' + $secondCold.Status + ')'
                $record.exitCode = $EXIT_NO_NEURAL_FRAME
                return $record
            }
            $second = [ordered]@{
                toggleToFirstNeuralFrameSeconds = Seconds-Between $againAt $secondCold.Entry.Time
                coldStartLine                   = $secondCold.Entry.Text
                plan                            = $null
            }
            $planLine = Find-LogLine -Context $context -Pattern 'Neural helper plan=(?<plan>[a-z-]+)' -Since $againAt
            if ($planLine -ne $null -and $planLine.Text -match 'plan=(?<plan>[a-z-]+)') {
                $second.plan = $Matches['plan']
            }
            Write-Progress-Line ('session ' + $Index + ': second toggle reached a neural frame in ' +
                $second.toggleToFirstNeuralFrameSeconds + ' s, plan=' + $second.plan)
        }

        # ---- harvest
        $startup = Find-LogLine -Context $context -Pattern $P.Startup
        $sessionStart = $accepted.Entry
        $receipt = Find-LogLine -Context $context -Pattern $P.Receipt -Since $toggleAt
        $entry = Find-LogLine -Context $context -Pattern $P.CacheEntry -Since $toggleAt
        $cacheDir = Find-LogLine -Context $context -Pattern $P.CacheDir
        $paceUnknown = Find-LogLine -Context $context -Pattern $P.PaceUnknown -Since $toggleAt
        $stopped = Find-LogLine -Context $context -Pattern $P.Stopped -Since $toggleAt

        $record.gpu = [ordered]@{ description = $null; driver = $null; generation = $null }
        if ($startup -ne $null -and $startup.Text -match $P.Startup) {
            $record.gpu.description = $Matches['gpu']
            $record.gpu.driver = $Matches['driver']
            $record.gpu.generation = $Matches['generation']
        }

        # Parsed from the whole line so `total` lands in the same ordered map as
        # the nine phases, in the order the player writes them.
        $phases = Parse-ColdStartFields $cold.Entry.Text

        $null = $pace.Entry.Text -match $P.Pace
        $paceRecord = [ordered]@{
            width          = [int]$Matches['width']
            height         = [int]$Matches['height']
            msPerFrame     = [double]$Matches['ms']
            frames         = [int]$Matches['frames']
            referenceScale = [double]$Matches['scale']
            geometriesKnown = [int]$Matches['geometries']
        }

        $sessionFrom = $null
        if ($sessionStart.Text -match $P.SessionStart) {
            # A replayed session ("replaying N retained segments") has no start
            # position in its line, so the group does not participate.
            if ($Matches.ContainsKey('from') -and $Matches['from']) { $sessionFrom = [double]$Matches['from'] }
        }

        # receipt.json is written beside the published payload; this is the
        # on-disk group the unit tests build and nothing has ever observed
        # from a player session.
        #
        # It is also the only place the whole nine-phase timeline exists for a
        # live session. The player merges the helper's five phases at
        # src/main.cpp:3616, after RunNeuralWorker returns - but the log line is
        # written from NoteNeuralFramePresented at the attach, seconds earlier,
        # and ClaimReport makes it once-only. So the log's cold-start line
        # structurally carries only the player's own phases, and the helper's
        # come from here. Both are recorded; neither is derived from the other.
        $receiptRecord = [ordered]@{
            logSummary  = $null
            entryPath   = $null
            receiptPath = $null
            onDisk      = $false
            bytes       = $null
            group       = @()
            jobId       = $null
            renderKey   = $null
            lockChecks  = $null
            lockSatisfied = $null
            frameCount  = $null
            verifiedNeuralFrames = $null
            coldStartSeconds = $null
        }
        if ($receipt -ne $null -and $receipt.Text -match $P.Receipt) { $receiptRecord.logSummary = $Matches['summary'] }
        if ($entry -ne $null -and $entry.Text -match $P.CacheEntry) {
            $entryPath = $Matches['entry']
            $receiptRecord.entryPath = $entryPath
            if ($entryPath -ne '(none)') {
                $directory = Split-Path -Parent $entryPath
                $candidate = Join-Path $directory 'receipt.json'
                $receiptRecord.receiptPath = $candidate
                if (Test-Path -LiteralPath $candidate) {
                    $receiptRecord.onDisk = $true
                    $receiptRecord.bytes = (Get-Item -LiteralPath $candidate).Length
                    $receiptRecord.group = @(Get-ChildItem -LiteralPath $directory -File | ForEach-Object { $_.Name + ':' + $_.Length })
                    try {
                        $json = Get-Content -LiteralPath $candidate -Raw | ConvertFrom-Json
                        $receiptRecord.jobId = $json.jobId
                        $receiptRecord.renderKey = $json.renderKey
                        $receiptRecord.lockSatisfied = $json.lock.satisfied
                        $receiptRecord.lockChecks = @($json.lock.checks).Count
                        $receiptRecord.frameCount = $json.result.frameCount
                        $receiptRecord.verifiedNeuralFrames = $json.result.verifiedNeuralFrames
                        # Microseconds on disk, seconds here, and a phase that
                        # did not run stays null rather than becoming 0.
                        $micro = $json.result.coldStartMicroseconds
                        $timeline = [ordered]@{}
                        foreach ($phase in @('total', 'request', 'preflight', 'launch', 'helperStart',
                                             'runtimeReady', 'neuralInit', 'featureArm', 'firstOutput', 'attach')) {
                            $value = $micro.$phase
                            if ($value -eq $null) { $timeline[$phase] = $null }
                            else { $timeline[$phase] = [Math]::Round([double]$value / 1000000.0, 6) }
                        }
                        $receiptRecord.coldStartSeconds = $timeline
                    } catch {
                        $receiptRecord.coldStartSeconds = $null
                    }
                }
            }
        }

        $record.timing = [ordered]@{
            launchToWindowSeconds          = Seconds-Between $launchAt $windowAt
            launchToRenderDeviceSeconds    = Seconds-Between $launchAt $readyAt
            windowToToggleSeconds          = Seconds-Between $windowAt $toggleAt
            # The acceptance number: a real keypress to a neural frame on screen.
            toggleToFirstNeuralFrameSeconds = Seconds-Between $toggleAt $firstNeuralAt
            toggleToSessionStartSeconds    = Seconds-Between $toggleAt $sessionStart.Time
            toggleToAttachLineSeconds      = Seconds-Between $toggleAt $attach.Entry.Time
            # The player's own request-to-picture stopwatch, for comparison with
            # the wall clock above. src/main.cpp NeuralColdStartRecord::Presented.
            playerReportedTotalSeconds     = $phases['total']
            injectionCostMilliseconds      = [Math]::Round(($toggle.Done - $toggle.At).TotalMilliseconds, 3)
        }
        $record.coldStart = $phases
        # Present only with -SecondToggle. The first toggle in a process always
        # pays a cold bring-up, so this is the number a resident helper changes.
        $record.secondToggle = $second
        $record.pace = $paceRecord
        $record.receipt = $receiptRecord
        $record.session = [ordered]@{
            accelerator          = $Accelerator
            acceleratorEvents    = $toggle.Events
            startPositionSeconds = $sessionFrom
            paceWasUnmeasured    = ($paceUnknown -ne $null)
            renderCompleted      = ($completionStatus -eq 'Matched')
            completionStatus     = $completionStatus
            renderCacheHit       = ((Find-LogLine -Context $context -Pattern $P.RenderStart -Since $toggleAt) -eq $null)
            cacheRoot            = $null
            launchLocal          = $launchAt.ToString('yyyy-MM-ddTHH:mm:ss.fff')
            windowLocal          = $windowAt.ToString('yyyy-MM-ddTHH:mm:ss.fff')
            toggleLocal          = $toggleAt.ToString('yyyy-MM-ddTHH:mm:ss.fff')
            firstNeuralFrameLocal = $firstNeuralAt.ToString('yyyy-MM-ddTHH:mm:ss.fff')
            toggleOffLocal       = $toggleOffAt.ToString('yyyy-MM-ddTHH:mm:ss.fff')
        }
        if ($cacheDir -ne $null -and $cacheDir.Text -match $P.CacheDir) { $record.session.cacheRoot = $Matches['root'] }

        $record.excerpts = [ordered]@{
            startup      = $(if ($startup -ne $null) { $startup.Raw } else { $null })
            renderDevice = $ready.Entry.Raw
            sessionStart = $sessionStart.Raw
            coldStart    = $cold.Entry.Raw
            attach       = $attach.Entry.Raw
            receipt      = $(if ($receipt -ne $null) { $receipt.Raw } else { $null })
            cacheEntry   = $(if ($entry -ne $null) { $entry.Raw } else { $null })
            pace         = $pace.Entry.Raw
            stopped      = $(if ($stopped -ne $null) { $stopped.Raw } else { $null })
        }
        return $record
    } finally {
        $shutdown = Stop-Player $context
        $record.player = [ordered]@{
            pid       = $process.Id
            cleanExit = $shutdown.Clean
            exitCode  = $shutdown.ExitCode
        }
        # The log is truncated by the next launch, so this session's copy is
        # kept beside the JSON rather than left to be overwritten. Bookkeeping
        # must never be what loses a measured session, so it cannot throw.
        $record.logCopy = $null
        if ($OutJson) {
            try {
                $outDirectory = Split-Path -Parent $OutJson
                if (-not $outDirectory) { $outDirectory = (Get-Location).ProviderPath }
                $target = Join-Path $outDirectory ([System.IO.Path]::GetFileNameWithoutExtension($OutJson) + '.session' + $Index + '.log')
                $text = Read-LogText $logPath
                if ($text) { Set-Content -LiteralPath $target -Value $text -Encoding UTF8; $record.logCopy = $target }
            } catch { }
        }
    }
}

# ---------------------------------------------------------------------- main
$resolvedPlayer = $null
$resolvedMedia = $null
try {
    $resolvedPlayer = (Resolve-Path -LiteralPath $Player).ProviderPath
    $resolvedMedia = (Resolve-Path -LiteralPath $Media).ProviderPath
} catch {
    Write-Progress-Line ('cannot resolve a path: ' + $_.Exception.Message)
}
if (-not $resolvedPlayer -or -not (Test-Path -LiteralPath $resolvedPlayer -PathType Leaf)) {
    Write-Progress-Line 'the -Player path is not a file'
    exit $EXIT_ARGUMENTS
}
if (-not $resolvedMedia -or -not (Test-Path -LiteralPath $resolvedMedia -PathType Leaf)) {
    Write-Progress-Line 'the -Media path is not a file'
    exit $EXIT_ARGUMENTS
}
if ($Sessions -lt 1) {
    Write-Progress-Line '-Sessions must be at least 1'
    exit $EXIT_ARGUMENTS
}
$runtimeDirectory = Join-Path (Split-Path -Parent $resolvedPlayer) 'neural-runtime'
if (-not (Test-Path -LiteralPath (Join-Path $runtimeDirectory 'NeuralWorker.exe'))) {
    Write-Progress-Line ('no neural-runtime beside the player: ' + $runtimeDirectory)
    exit $EXIT_ARGUMENTS
}
if ($OutJson) {
    $outDirectory = Split-Path -Parent $OutJson
    if ($outDirectory -and -not (Test-Path -LiteralPath $outDirectory)) {
        [void](New-Item -ItemType Directory -Path $outDirectory -Force)
    }
}

# A photo has no timeline for a session to follow - the same toggle renders a
# single frame instead - and a clip too short to reach 120 rendered frames can
# never produce a pace line. Both would come out of the scrape as "the key did
# not arrive", which is a lie about the instrument rather than about the clip.
# So the geometry this record claims is probed here, from the same ffprobe the
# player itself uses, and travels with the numbers.
$probe = [ordered]@{ width = $null; height = $null; fps = $null; durationSeconds = $null }
$ffprobe = Join-Path (Split-Path -Parent $resolvedPlayer) 'ffprobe.exe'
if (-not (Test-Path -LiteralPath $ffprobe)) {
    Write-Progress-Line ('no ffprobe.exe beside the player: ' + $ffprobe)
    exit $EXIT_ARGUMENTS
}
$probeLines = & $ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate `
    -show_entries format=duration -of default=noprint_wrappers=1 $resolvedMedia 2>$null
foreach ($line in @($probeLines)) {
    if ($line -match '^width=(\d+)$') { $probe.width = [int]$Matches[1] }
    elseif ($line -match '^height=(\d+)$') { $probe.height = [int]$Matches[1] }
    elseif ($line -match '^r_frame_rate=(\d+)/(\d+)$' -and [int]$Matches[2] -ne 0) {
        $probe.fps = [Math]::Round([double]$Matches[1] / [double]$Matches[2], 3)
    }
    elseif ($line -match '^duration=([0-9.]+)$') { $probe.durationSeconds = [double]$Matches[1] }
}
if (-not $probe.width -or -not $probe.fps -or -not $probe.durationSeconds) {
    Write-Progress-Line 'ffprobe found no video stream with a frame rate and a duration in -Media'
    exit $EXIT_ARGUMENTS
}
# 120 rendered frames is the player's own floor for reporting a pace
# (kMinPaceFrames in src/main.cpp), which is 4 s of 30 fps video, and the
# session also has to buffer its start lead before it attaches.
$minimumSeconds = 8.0
if ($probe.durationSeconds -lt $minimumSeconds) {
    Write-Progress-Line ('-Media is ' + $probe.durationSeconds + ' s; a session needs at least ' + $minimumSeconds + ' s to reach 120 rendered frames')
    exit $EXIT_ARGUMENTS
}

$playerItem = Get-Item -LiteralPath $resolvedPlayer
$mediaItem = Get-Item -LiteralPath $resolvedMedia
$osInfo = Get-CimInstance Win32_OperatingSystem
$gpuInfo = @(Get-CimInstance Win32_VideoController | ForEach-Object { $_.Name + ' driver=' + $_.DriverVersion })

$records = New-Object System.Collections.ArrayList
$worstExit = $EXIT_OK
for ($index = 1; $index -le $Sessions; $index++) {
    $clear = ($FreshProfile -eq 'Each') -or (($FreshProfile -eq 'First') -and ($index -eq 1))
    Write-Progress-Line ('session ' + $index + ' of ' + $Sessions + ' (' + $(if ($clear) { 'cold, profile cleared' } else { 'warm' }) + ')')
    $record = Invoke-PlayerSession -Index $index -PlayerPath $resolvedPlayer -MediaPath $resolvedMedia -Clear $clear
    [void]$records.Add($record)
    if ($record.exitCode -ne $EXIT_OK) {
        Write-Progress-Line ('session ' + $index + ' FAILED (' + $record.exitCode + '): ' + $record.failure)
        if ($worstExit -eq $EXIT_OK) { $worstExit = $record.exitCode }
    }
    if ($index -lt $Sessions) { Start-Sleep -Seconds 2 }
}

$ok = @($records | Where-Object { $_.exitCode -eq $EXIT_OK })
$summary = [ordered]@{
    sessionsRequested = $Sessions
    sessionsPassed    = $ok.Count
    toggleToFirstNeuralFrameSeconds = $null
    playerReportedTotalSeconds      = $null
    paceMsPerFrame                  = $null
    paceReferenceScale              = $null
    receiptsOnDisk                  = @($ok | Where-Object { $_.receipt.onDisk }).Count
    coldStartLinesObserved          = @($ok | Where-Object { $_.coldStart -ne $null }).Count
}
if ($ok.Count -gt 0) {
    $summary.toggleToFirstNeuralFrameSeconds = Summarize-Numbers @($ok | ForEach-Object { [double]$_.timing.toggleToFirstNeuralFrameSeconds })
    $totals = @($ok | Where-Object { $_.timing.playerReportedTotalSeconds -ne $null } | ForEach-Object { [double]$_.timing.playerReportedTotalSeconds })
    if ($totals.Count -gt 0) { $summary.playerReportedTotalSeconds = Summarize-Numbers $totals }
    $summary.paceMsPerFrame = Summarize-Numbers @($ok | ForEach-Object { [double]$_.pace.msPerFrame })
    $summary.paceReferenceScale = Summarize-Numbers @($ok | ForEach-Object { [double]$_.pace.referenceScale })
}

$document = [ordered]@{
    schema    = 1
    tool      = 'tools/verification/player_session.ps1'
    generated = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    host      = [ordered]@{
        computer   = $env:COMPUTERNAME
        os         = $osInfo.Caption
        version    = $osInfo.Version
        powershell = $PSVersionTable.PSVersion.ToString()
        adapters   = $gpuInfo
    }
    player    = [ordered]@{
        path     = $resolvedPlayer
        bytes    = $playerItem.Length
        built    = $playerItem.LastWriteTime.ToString('yyyy-MM-ddTHH:mm:ss')
        runtime  = $runtimeDirectory
    }
    media     = [ordered]@{
        path            = $resolvedMedia
        bytes           = $mediaItem.Length
        width           = $probe.width
        height          = $probe.height
        fps             = $probe.fps
        durationSeconds = $probe.durationSeconds
    }
    options   = [ordered]@{
        toggleAfterSeconds = $ToggleAfterSeconds
        runSeconds         = $RunSeconds
        sessions           = $Sessions
        freshProfile       = $FreshProfile
        accelerator        = $Accelerator
        dropRenderCache    = [bool]$DropRenderCache
        settleSeconds      = $SettleSeconds
        note               = $Note
    }
    summary   = $summary
    sessions  = @($records)
    outcome   = [ordered]@{
        ok       = ($worstExit -eq $EXIT_OK)
        exitCode = $worstExit
    }
}

$json = $document | ConvertTo-Json -Depth 12
Write-Output $json
if ($OutJson) {
    Set-Content -LiteralPath $OutJson -Value $json -Encoding UTF8
    Write-Progress-Line ('wrote ' + $OutJson)
}
exit $worstExit
