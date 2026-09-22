<#
.SYNOPSIS
  Turns neural rendering on in a real player session without SendInput.

.DESCRIPTION
  player_session.ps1 injects the accelerator through SendInput, which Windows
  refuses when the caller is not the foreground-capable interactive session -
  an unattended agent, a locked workstation, a service. This posts the menu
  command straight to the window instead (WM_COMMAND / IDM_NEURAL_RENDERING),
  which reaches the same ToggleNeuralRendering() and needs no focus at all.

  It is a debugging instrument, not a replacement: it cannot tell you that the
  key binding works, only what the render does once the command arrives.

.EXAMPLE
  ./tools/verification/drive-neural-toggle.ps1 `
      -Player build-upscaling/Release/DLSSVideoPlayer.exe `
      -Media  path/to/source.mp4
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Player,
    [Parameter(Mandatory = $true)][string]$Media,
    [double]$SettleSeconds = 6.0,
    [double]$RunSeconds = 45.0
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Win -Name Native -MemberDefinition @'
[DllImport("user32.dll", SetLastError=true)]
public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
'@

# app_menu::IDM_NEURAL_RENDERING
$IDM_NEURAL_RENDERING = 300
$WM_COMMAND = 0x0111

$playerPath = (Resolve-Path -LiteralPath $Player).Path
$mediaPath  = (Resolve-Path -LiteralPath $Media).Path
$logPath = Join-Path (Split-Path -Parent $playerPath) `
    ((Get-Item -LiteralPath $playerPath).BaseName + '.log')
if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }

$process = Start-Process -FilePath $playerPath -ArgumentList @($mediaPath) -PassThru
try {
    Start-Sleep -Seconds $SettleSeconds
    $process.Refresh()
    if ($process.HasExited) { throw 'The player exited before the toggle.' }
    if ($process.MainWindowHandle -eq [IntPtr]::Zero) { throw 'The player has no main window.' }

    if (-not [Win.Native]::PostMessageW($process.MainWindowHandle, $WM_COMMAND,
            [IntPtr]$IDM_NEURAL_RENDERING, [IntPtr]::Zero)) {
        throw "PostMessage failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
    }
    Write-Output "toggle posted; running $RunSeconds s"
    Start-Sleep -Seconds $RunSeconds
}
finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(8000)) { $process.Kill() }
    }
}

# The lines that say whether a neural frame was ever produced.
Get-Content -LiteralPath $logPath |
    Select-String -Pattern 'Active neural session|neural render receipt|cache (hit|miss)|feature 18|ended early|attached|buffer' |
    ForEach-Object { $_.Line }
