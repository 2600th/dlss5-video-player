param([string]$Process = 'DLSSVideoPlayer',
      [string]$DialogTitle = 'Frame Generation',
      [int]$TimeoutSeconds = 600,
      # Yes converts; No dismisses and returns. A refusal dialog is OK-only, so
      # either answer closes it and no conversion is waited for.
      [ValidateSet('Yes','No')][string]$Answer = 'Yes')
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
[DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder text, int max);
'@
$IDM_FRAME_GENERATION = 309
$WM_COMMAND = 0x0111
# The confirmation is Yes/No with No focused, so Enter cannot commit a
# minutes-long GPU job by accident; this script has to answer IDYES.
$IDYES = 6

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
# The body text is the evidence: it names the multiple, the rates and - when the
# generated rate does not divide the refresh - the two hold lengths. Read before
# answering, because the dialog is gone immediately after.
$body = New-Object System.Text.StringBuilder 2048
[void][Win.Drive]::GetWindowTextW([Win.Drive]::GetDlgItem($dialog, 0xFFFF), $body, 2048)
"--- dialog body ---"
$body.ToString()
"--- end body ---"
$IDNO = 7
$IDOK = 1
if ($Answer -eq 'Yes') {
    [void][Win.Drive]::PostMessage($dialog, $WM_COMMAND, [IntPtr]$IDYES, [IntPtr]::Zero)
} else {
    # A refusal is OK-only and an offer is Yes/No; a MessageBox ignores a
    # command id it has no button for, so posting both closes either shape.
    [void][Win.Drive]::PostMessage($dialog, $WM_COMMAND, [IntPtr]$IDNO, [IntPtr]::Zero)
    [void][Win.Drive]::PostMessage($dialog, $WM_COMMAND, [IntPtr]$IDOK, [IntPtr]::Zero)
    "answered No"
    exit 0
}
"answered Yes; waiting up to $TimeoutSeconds s"

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
