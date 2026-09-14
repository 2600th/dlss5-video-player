<#
.SYNOPSIS
Downloads the camera-original benchmark sources into build-upscaling/camera-original.

.DESCRIPTION
The `real-*` corpus clips are NR-processed captures: the player's own DLSS-NR
output, screen-captured, h264-encoded twice and lanczos-upscaled before the pass
under test renders them again. The `orig-*` clips exist to close that gap, and
they start from the publisher's own release of the same titles.

Those releases are copyrighted trailers. They are NOT committed and NOT
redistributed - this script fetches them, `tools/benchmark/corpus.py` cuts the
labelled spans out of them, and each `orig-*` builder skips itself when its
source is absent, so a checkout without them still builds a complete corpus.

Unlike tools/fetch_*.ps1, nothing here is SHA-pinned: a streaming site re-encodes
its own files, so a byte hash would fail for a reason that is not a problem. What
IS pinned is the video id, the format id, and the geometry and frame rate this
script verifies after download. The real integrity check lives downstream in the
per-clip frame digests that corpus.py records in manifest.json, so
`python tools/benchmark/corpus.py --check` proves whether a rebuild produced the
same clips the committed labels were verified against.

.PARAMETER Destination
Where to place the sources. Defaults to build-upscaling/camera-original.

.PARAMETER YtDlp
yt-dlp executable. Defaults to external/youtube/yt-dlp.exe, staged by
tools/fetch_youtube_helpers.ps1.

.PARAMETER Force
Re-download sources that are already present.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools/benchmark/fetch_camera_original.ps1
#>
[CmdletBinding()]
param(
    [string]$Destination,
    [string]$YtDlp,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Destination) { $Destination = Join-Path $repositoryRoot 'build-upscaling\camera-original' }
if (-not $YtDlp) { $YtDlp = Join-Path $repositoryRoot 'external\youtube\yt-dlp.exe' }
$ffprobe = Join-Path $repositoryRoot 'external\ffmpeg\bin\ffprobe.exe'

# Name, selector and expectations. The two 1440p VP9 sources are the upstreams of
# docs/media/neural-comparison-demo.mp4 and are documented in docs/media/README.md;
# the third is here for the cross-dissolve neither of them contains.
$sources = @(
    [pscustomobject]@{
        File     = 'godfather-50th.webm'
        Selector = 'https://www.youtube.com/watch?v=UaVTIH8mujA'
        Format   = '271'
        Width    = 2560
        Height   = 1440
        Rate     = '24000/1001'
        Why      = 'THE GODFATHER 50th Anniversary Trailer (Paramount Pictures) - upstream of the demo capture; orig-film-cuts-a/b, orig-film-fade, orig-faces'
    },
    [pscustomobject]@{
        File     = 'gtavi-extended.webm'
        Selector = 'https://www.youtube.com/watch?v=uphThaa97ig'
        Format   = '271'
        Width    = 2560
        Height   = 1440
        Rate     = '30/1'
        Why      = 'Grand Theft Auto VI: An Extended Look (Netflix) - upstream of the demo capture; orig-game-cuts, orig-game-motion'
    },
    [pscustomobject]@{
        File     = 'cand-lawrence-arabia.mp4'
        Selector = 'ytsearch1:Lawrence of Arabia official trailer restored'
        Format   = 'bestvideo[height<=1440][ext=mp4]/bestvideo[height<=1440]'
        Width    = 1920
        Height   = 1038
        Rate     = '24000/1001'
        Why      = 'Lawrence of Arabia restored trailer (Sony/Columbia) - the only verified cross-dissolve; orig-dissolve'
    }
)

foreach ($tool in @($YtDlp, $ffprobe)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required tool is missing: $tool (run tools/fetch_youtube_helpers.ps1 and tools/fetch_ffmpeg_helpers.ps1)"
    }
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$failures = 0
foreach ($source in $sources) {
    $path = Join-Path $Destination $source.File
    if ((Test-Path -LiteralPath $path) -and -not $Force) {
        Write-Host ("present   {0}" -f $source.File)
    }
    else {
        Write-Host ("fetching  {0}  <- {1} (format {2})" -f $source.File, $source.Selector, $source.Format)
        $template = Join-Path $Destination ([IO.Path]::GetFileNameWithoutExtension($source.File) + '.%(ext)s')
        & $YtDlp $source.Selector '--no-playlist' '--no-warnings' '-f' $source.Format '-o' $template
        if ($LASTEXITCODE -ne 0) {
            Write-Warning ("yt-dlp failed for {0} (exit {1})" -f $source.File, $LASTEXITCODE)
            $failures++
            continue
        }
    }

    if (-not (Test-Path -LiteralPath $path)) {
        Write-Warning ("expected {0} after download and it is not there; the site may have changed its formats" -f $path)
        $failures++
        continue
    }

    # Geometry and rate decide whether the committed frame labels still apply: the
    # spans in corpus.py are source frame indices, so a different rate is a
    # different clip even when the content matches.
    $probed = & $ffprobe '-v' 'error' '-select_streams' 'v:0' '-show_entries' 'stream=width,height,r_frame_rate' '-of' 'csv=p=0' $path
    $fields = ($probed | Select-Object -First 1).Trim() -split ','
    $ok = ($fields.Count -eq 3) -and ([int]$fields[0] -eq $source.Width) -and
          ([int]$fields[1] -eq $source.Height) -and ($fields[2] -eq $source.Rate)
    if ($ok) {
        Write-Host ("verified  {0}  {1}x{2} @ {3}  - {4}" -f $source.File, $fields[0], $fields[1], $fields[2], $source.Why)
    }
    else {
        Write-Warning ("{0} is {1} but the labels were verified against {2}x{3} @ {4}. The orig-* clips built from it will not match the committed digests; re-check the labels before citing them." -f `
            $source.File, ($fields -join 'x'), $source.Width, $source.Height, $source.Rate)
        $failures++
    }
}

Write-Host ''
Write-Host ("destination: {0}" -f $Destination)
Write-Host 'next: python tools/benchmark/corpus.py   then   python tools/benchmark/corpus.py --check'
if ($failures -gt 0) {
    Write-Warning ("{0} source(s) unusable; the orig-* clips depending on them will be skipped by corpus.py" -f $failures)
    exit 1
}
exit 0
