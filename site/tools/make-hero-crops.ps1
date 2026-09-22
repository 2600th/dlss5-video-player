#Requires -Version 5.1
<#
.SYNOPSIS
    Regenerates the hero comparison crops from the committed screenshots.

.DESCRIPTION
    The hero wipes between one frame rendered two ways: Grand Theft Auto VI
    Trailer 2, source frame 1940 (64.67 s), and the same frame from the render
    the shipping player wrote for that source. Both full 2560x1440 frames are
    committed under docs/screenshots/current/ (gta6-lucia-*.jpg), so the hero can
    be rebuilt from the tree.

    The plates are a native 1920x1080 window of the active picture - the
    trailer is letterboxed, rows 144-1295 carry picture - taken at x=0 so the
    face sits at 57% of the width, where the page's seam rests (55%), and the
    flat background on the left is free for the headline. The previous plates
    were a 920x518 crop of a player-window capture, stretched full-bleed.

    Both frames are cropped with identical parameters. No scaling, no colour
    adjustment, no sharpening: the only operation is the crop and one encode.
    The script verifies afterwards that the encode preserved the difference
    between the two, because an encode that smoothed it away would make the
    hero misrepresent the product in its favour.

    Requires ffmpeg. Run from anywhere; paths resolve against the repository.
#>
[CmdletBinding()]
param([switch]$Force)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$srcDir = [IO.Path]::Combine($repoRoot, 'docs', 'screenshots', 'current')
$outDir = [IO.Path]::Combine($repoRoot, 'site', 'src', 'assets', 'hero')

# See the description above. Source-pixel coordinates in the 2560x1440 frame.
$crop = @{ X = 0; Y = 144; W = 1920; H = 1080 }
$quality = 2   # ffmpeg mjpeg scale, 2 = highest practical quality

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$pairs = @(
    @{ In = 'gta6-lucia-original.jpg'; Out = 'hero-original.jpg' }
    @{ In = 'gta6-lucia-neural.jpg';   Out = 'hero-neural.jpg' }
)

foreach ($p in $pairs) {
    $in = Join-Path $srcDir $p.In
    $out = Join-Path $outDir $p.Out
    if ((Test-Path $out) -and -not $Force) { Write-Host "  exists $($p.Out) (use -Force to regenerate)"; continue }
    & ffmpeg -hide_banner -loglevel error -y -i $in `
        -vf "crop=$($crop.W):$($crop.H):$($crop.X):$($crop.Y)" -q:v $quality -frames:v 1 $out
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed on $($p.In)" }
    Write-Host "  wrote  $($p.Out)"
}

# Verify the encode kept the render's difference intact.
# The stats go to a file rather than stderr: Windows PowerShell wraps a native
# command's stderr in error records, which turns a successful measurement into
# a terminating error under $ErrorActionPreference = 'Stop'.
# A bare filename, written from a scratch directory: ffmpeg's filter-graph
# parser treats the colon in a Windows drive path as an option separator.
$scratch = [IO.Path]::Combine([IO.Path]::GetTempPath(), "hero-psnr-$PID")
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$originalCrop = Join-Path $outDir 'hero-original.jpg'
$neuralCrop = Join-Path $outDir 'hero-neural.jpg'
Push-Location $scratch
try {
    & ffmpeg -hide_banner -loglevel error -y -i $originalCrop -i $neuralCrop `
        -filter_complex '[0:v][1:v]psnr=stats_file=psnr.txt' -f null - | Out-Null
} finally { Pop-Location }
$statsFile = Join-Path $scratch 'psnr.txt'
if (-not (Test-Path $statsFile)) { throw 'Could not measure the crops; refusing to certify them.' }
$stats = Get-Content $statsFile -Raw
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
if ($stats -notmatch 'psnr_avg:([0-9.]+)') { throw "Unexpected psnr output: $stats" }
$psnr = [double]$Matches[1]

Write-Host ''
Write-Host "  PSNR between the two crops: $([math]::Round($psnr,2)) dB"
if ($psnr -gt 34) {
    throw "The crops differ by only $([math]::Round($psnr,2)) dB; the full source frames differ by far more. The encode has smoothed the render's effect away and the hero would understate it."
}
Write-Host '  the crop preserves the difference the source captures carry.' -ForegroundColor Green

# Provenance, so a later reader knows exactly what these files are.
@{
    generatedBy = 'site/tools/make-hero-crops.ps1'
    sources     = @('docs/screenshots/current/gta6-lucia-original.jpg', 'docs/screenshots/current/gta6-lucia-neural.jpg')
    provenance  = 'docs/screenshots/README.md'
    crop        = "x=$($crop.X) y=$($crop.Y) w=$($crop.W) h=$($crop.H) in the 2560x1440 frame"
    operations  = 'crop only; no scaling, colour adjustment or sharpening'
    cropBoxChosenBy = 'the active (non-letterbox) picture, with the face at the page seam'
    psnrBetweenCrops = [math]::Round($psnr, 2)
    note        = 'Grand Theft Auto VI Trailer 2, source frame 1940, and the same frame from the v0.25.0 player render. See docs/screenshots/README.md.'
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outDir 'provenance.json') -Encoding utf8
Write-Host '  wrote  provenance.json'
