<#
.SYNOPSIS
  CPU cost of plain playback, for comparing two player builds.

.DESCRIPTION
  The message loop used to call Sleep(0) while playing - a yield, not a block -
  so it free-ran and every wasted iteration re-walked the live coverage spans
  under their mutex, read waveOutGetPosition under the audio producer's lock,
  and on a YouTube source built a NeuralCacheManager twice. Making the
  swapchain waitable and blocking on it instead is supposed to remove that.

  This measures the claim directly: launch a build on a local file, let
  playback settle, then sample the process's own accumulated processor time
  over a fixed window. CPU seconds per wall second is the number - 1.00 means
  one core fully busy.

  It also reads back the player's log to confirm the session actually played
  rather than stalling, which is the regression a frame-latency wait could
  plausibly introduce.

.EXAMPLE
  ./tools/verification/measure-idle-cost.ps1 `
      -Player build-upscaling/Release/DLSSVideoPlayer.exe `
      -Media  build/media-refresh-20260912/godfather-source.mp4
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Player,
    [Parameter(Mandatory = $true)][string]$Media,
    # Playback needs to reach a steady state first: the first seconds are the
    # decoder starting, the audio child spawning and the window laying out.
    [double]$SettleSeconds = 6.0,
    [double]$SampleSeconds = 15.0
)

$ErrorActionPreference = 'Stop'

$playerPath = (Resolve-Path -LiteralPath $Player).Path
$mediaPath = (Resolve-Path -LiteralPath $Media).Path
$logPath = Join-Path (Split-Path -Parent $playerPath) `
    ((Get-Item -LiteralPath $playerPath).BaseName + '.log')

# A previous run's log would make the "did it play" check meaningless.
if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }

$process = Start-Process -FilePath $playerPath -ArgumentList @($mediaPath) -PassThru
try {
    Start-Sleep -Seconds $SettleSeconds
    $process.Refresh()
    if ($process.HasExited) { throw "The player exited during settle; it never reached playback." }

    $cpuBefore = $process.TotalProcessorTime
    $wallBefore = [Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $SampleSeconds
    $wallBefore.Stop()
    $process.Refresh()
    if ($process.HasExited) { throw "The player exited mid-sample." }
    $cpuAfter = $process.TotalProcessorTime

    $cpuSeconds = ($cpuAfter - $cpuBefore).TotalSeconds
    $wallSeconds = $wallBefore.Elapsed.TotalSeconds
    $cores = [Environment]::ProcessorCount

    [pscustomobject]@{
        Player          = $playerPath
        CpuSecondsPerWallSecond = [Math]::Round($cpuSeconds / $wallSeconds, 3)
        PercentOfOneCore        = [Math]::Round(100.0 * $cpuSeconds / $wallSeconds, 1)
        PercentOfMachine        = [Math]::Round(100.0 * $cpuSeconds / ($wallSeconds * $cores), 2)
        WallSeconds             = [Math]::Round($wallSeconds, 2)
    }
}
finally {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
}

# Evidence that it played rather than stalled: the decoder logs its open, and a
# stalled presentation would leave the session without ever advancing.
if (Test-Path -LiteralPath $logPath) {
    $log = Get-Content -LiteralPath $logPath -Raw
    $opened = [regex]::Matches($log, 'Opened|decoder|FFmpeg PCM/WaveOut path started').Count
    Write-Output "log: $logPath ($opened progress lines)"
    $log -split "`n" | Select-String -Pattern 'Playback health|WaveOut path started|frozen|stall' |
        Select-Object -First 5 | ForEach-Object { Write-Output "  $_" }
}
