#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the demonstration video's poster from a frame of the video itself.

.DESCRIPTION
    docs/media/neural-comparison-poster.jpg is the demo's title card, and it is
    lettered in the demo's own green - a chartreuse badge, a green wordmark and
    a green rule. On the page that card renders about 1126x633, which put a
    second ink on a surface whose whole visual argument is one.

    So the poster is a frame of the footage instead: frame 460, the GTA VI
    playback take, cropped to the player window. The caption panel the demo
    composites down the left of every frame is cropped away; what is left is
    the application doing the thing the section is about, with its status line
    reporting a live render. Nothing is added, retouched or recoloured.

    The source video is unchanged - this only reads it.

    Requires ffmpeg.
#>
[CmdletBinding()]
param([switch]$Force)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$source = [IO.Path]::Combine($repoRoot, 'docs', 'media', 'neural-comparison-demo.mp4')
$outDir = [IO.Path]::Combine($repoRoot, 'site', 'src', 'assets', 'demo')
$out = Join-Path $outDir 'demo-poster.jpg'

# Frame 460 of 678: the playback take, after the title card and with no badge
# overlaid. The crop is the player window, located by its title bar.
$frame = 460
$crop = @{ X = 464; Y = 75; W = 1438; H = 930 }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if ((Test-Path $out) -and -not $Force) {
    Write-Host '  exists demo-poster.jpg (use -Force to regenerate)'
    return
}

& ffmpeg -hide_banner -loglevel error -y -i $source `
    -vf "select=eq(n\,$frame),crop=$($crop.W):$($crop.H):$($crop.X):$($crop.Y)" `
    -frames:v 1 -q:v 3 $out
if ($LASTEXITCODE -ne 0) { throw 'ffmpeg failed while building the demo poster.' }
Write-Host "  wrote  demo-poster.jpg"

# The poster must not be the title card: that card is the reason this script
# exists, and its green is what the check looks for. A frame of footage has no
# large field of that hue.
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $out
$green = 0; $total = 0
for ($y = 0; $y -lt $bmp.Height; $y += 7) {
    for ($x = 0; $x -lt $bmp.Width; $x += 7) {
        $c = $bmp.GetPixel($x, $y); $total++
        # the card's chartreuse: strongly green-dominant and bright
        if ($c.G -gt 120 -and $c.G -gt $c.B + 50 -and $c.G -gt $c.R + 30) { $green++ }
    }
}
$ratio = [math]::Round(100 * $green / $total, 2)
$bmp.Dispose()

Write-Host "  chartreuse coverage: $ratio%"
if ($ratio -gt 4) {
    Remove-Item $out -Force
    throw "The poster is $ratio% chartreuse, which means the title card was captured rather than the footage. Pick a different frame."
}
Write-Host '  the poster is footage, not the title card.' -ForegroundColor Green

@{
    generatedBy = 'site/tools/make-demo-poster.ps1'
    source      = 'docs/media/neural-comparison-demo.mp4'
    provenance  = 'docs/media/README.md and docs/screenshots/README.md'
    frame       = $frame
    crop        = "x=$($crop.X) y=$($crop.Y) w=$($crop.W) h=$($crop.H) of 1920x1080"
    operations  = 'select one frame, crop to the player window, one JPEG encode'
    why         = 'the shipped title card is lettered in the demo green, which put a second ink on a one-ink page'
    shows       = 'Grand Theft Auto VI playing with neural rendering attached, status line reporting the live render'
    credit      = 'Rockstar Games, via Netflix Now Playing'
    chartreuseCoverage = $ratio
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outDir 'provenance.json') -Encoding utf8
Write-Host '  wrote  provenance.json'
