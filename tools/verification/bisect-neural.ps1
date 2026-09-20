<#
  git bisect run script for "neural rendering produces no frames".
  Exit 0 = good (frames were rendered), 1 = bad, 125 = cannot test.
#>
$ErrorActionPreference = 'Continue'
$cm = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

& $cm -S . -B build-upscaling *> $null
& $cm --build build-upscaling --config Release --target DLSSVideoPlayer NeuralWorker -- /m /v:q *> $null
if (-not (Test-Path 'build-upscaling\Release\DLSSVideoPlayer.exe')) { exit 125 }
if (-not (Test-Path 'build-upscaling\Release\neural-runtime\NeuralWorker.exe')) { exit 125 }

# A stale cache entry would answer the toggle without rendering anything.
Remove-Item -Recurse -Force 'build-upscaling\Release\cache\v1\renders' -ErrorAction SilentlyContinue

$out = & .\tools\verification\drive-neural-toggle.ps1 `
    -Player build-upscaling\Release\DLSSVideoPlayer.exe `
    -Media build\media-refresh-20260912\godfather-source.mp4 `
    -RunSeconds 45 2>&1 | Out-String

$rendered = $out -match 'failure=none frames=(\d+)/' -and [int]$Matches[1] -gt 0
Write-Host "---- $(git rev-parse --short HEAD): $(if ($rendered) {'GOOD'} else {'BAD'})"
if ($rendered) { exit 0 } else { exit 1 }
