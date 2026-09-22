#Requires -Version 5.1
<#
.SYNOPSIS
    Builds the demonstration video's poster from a frame of the video itself.

.DESCRIPTION
    The poster is a frame of the demonstration itself: frame 110 of 591, the
    opening Matrix comparison with the divider already across Trinity's face
    and both side labels on screen. It is the whole 1920x1080 frame - the
    demonstration is full-bleed footage now, so there is no player window or
    caption panel to crop away. Nothing is added, retouched or recoloured.

    The chartreuse check below dates from the previous cut, whose title card
    was lettered in green; it stays because it still proves the poster is
    footage rather than a card.

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

# Frame 110 of 591: the opening comparison, divider settled on the face.
$frame = 110

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if ((Test-Path $out) -and -not $Force) {
    Write-Host '  exists demo-poster.jpg (use -Force to regenerate)'
    return
}

& ffmpeg -hide_banner -loglevel error -y -i $source `
    -vf "select=eq(n\,$frame)" `
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
    operations  = 'select one frame, one JPEG encode'
    why         = 'the poster is the first comparison a visitor sees, so it is a comparison rather than a card'
    shows       = 'The Matrix, one paused frame split down the face: source on the left, the player render on the right'
    credit      = 'Warner Bros. (The Matrix | 4K Trailer)'
    chartreuseCoverage = $ratio
} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outDir 'provenance.json') -Encoding utf8
Write-Host '  wrote  provenance.json'
