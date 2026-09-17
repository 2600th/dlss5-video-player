param([string]$Process = 'DLSSVideoPlayer',
      [string]$DialogTitle = 'Generate frames (higher frame rate)...',
      [int]$TimeoutSeconds = 600)
# Drives the frame-generation conversion from outside the player: posts the menu
# command, answers the confirmation, and waits for the worker to finish. The
# player is a GUI app with a modal confirmation in the middle of the flow, so a
# bare PostMessage would sit against a dialog nobody dismissed.
#
# The dialog is found by its exact title rather than by enumerating windows and
# matching the owner: MessageBoxW's title is the menu label, which is a fact this
# script can state, while EnumWindows through a PowerShell delegate proved
# unreliable here and cost more time than it saved.
Add-Type -Namespace Win -Name Drive -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
'@
$IDM_FRAME_GENERATION = 309
$WM_COMMAND = 0x0111
$IDOK = 1

$p = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw "no window for $Process" }
$log = Join-Path (Split-Path $p.Path) 'DLSSVideoPlayer.log'

[void][Win.Drive]::PostMessage($p.MainWindowHandle, $WM_COMMAND, [IntPtr]$IDM_FRAME_GENERATION, [IntPtr]::Zero)
"posted WM_COMMAND IDM_FRAME_GENERATION"

# The capability probe creates and releases an NGX feature before the dialog
# appears, so allow for it rather than polling once.
$dialog = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 250
    $dialog = [Win.Drive]::FindWindowW('#32770', $DialogTitle)
}
if ($dialog -eq [IntPtr]::Zero) { throw "no confirmation dialog appeared" }
"confirmation dialog found: $dialog"
[void][Win.Drive]::PostMessage($dialog, $WM_COMMAND, [IntPtr]$IDOK, [IntPtr]::Zero)
"answered OK; waiting up to $TimeoutSeconds s"

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process -Id $p.Id -ErrorAction SilentlyContinue)) { throw 'player exited during conversion' }
    Start-Sleep -Milliseconds 500
    if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch 'Frame generation finished' -Quiet)) {
        'conversion finished'
        exit 0
    }
}
throw "conversion did not finish within $TimeoutSeconds s"
