#Requires -Version 5.1
<#
.SYNOPSIS
    Regenerates the hero comparison crops from one frame rendered two ways.

.DESCRIPTION
    The hero wipes between one frame of 007 First Light - Story Trailer, source
    frame 1122 (18.72 s), and the same frame from the whole-video render the
    player wrote for that source at its default neural settings (render cache
    key 76561c1d..., 5,784 of 5,784 frames verified). The full 2560x1440
    frames are the demonstration video's inputs, bond-1122-original.png and
    bond-1122-neural.png, written by tools/demo-video/prepare-inputs.py into
    tools/demo-video/public/ (not committed; regenerate them there, or point
    -SourceDir at a checkout that has them).

    They are not committed, so the script ties them to what is: the comparison
    still docs/media/stills/007-first-light-bond.png is two unscaled 700x880
    crops of these same two frames, and before cutting anything the script
    checks that each frame, cropped at the still's recorded box, is the still's
    half pixel for pixel. A frame that fails is refused.

    The plates are a native 1920x1080 window of the active picture - the
    trailer is letterboxed, rows 176-1264 carry picture - taken at x=426 so the
    face spans about 43-72% of the width, where the page's seam rests (55%),
    with the dark panelling on the left free for the headline.

    Both frames are cropped with identical parameters. No scaling, no colour
    adjustment, no sharpening: the only operations are the crop and one JPEG
    encode. Afterwards the script measures the two encoded plates against each
    other and against the same two windows of the lossless frames, and refuses
    a pair the encode has made more alike, because a hero that smoothed the
    render's effect away would misrepresent the product in its favour.

    Requires ffmpeg. Run from anywhere; paths resolve against the repository.
#>
[CmdletBinding()]
param(
    [string]$SourceDir,
    [switch]$Force,
    [double]$MaxPairGainDb = 0.5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $SourceDir) { $SourceDir = [IO.Path]::Combine($repoRoot, 'tools', 'demo-video', 'public') }
$outDir = [IO.Path]::Combine($repoRoot, 'site', 'src', 'assets', 'hero')
$still = [IO.Path]::Combine($repoRoot, 'docs', 'media', 'stills', '007-first-light-bond.png')
$stillRecord = [IO.Path]::Combine($repoRoot, 'docs', 'media', 'stills', '007-first-light-bond.provenance.json')

# See the description above. Source-pixel coordinates in the 2560x1440 frame.
$crop = @{ X = 426; Y = 180; W = 1920; H = 1080 }
$quality = 2   # ffmpeg mjpeg scale, 2 = highest practical quality

$inputs = @{
    original = Join-Path $SourceDir 'bond-1122-original.png'
    neural   = Join-Path $SourceDir 'bond-1122-neural.png'
}
foreach ($path in $inputs.Values) { if (-not (Test-Path $path)) { throw "Source frame missing: $path" } }

# ffmpeg's filter-graph parser reads the colon of a Windows drive path as an
# option separator, and Windows PowerShell turns a native command's stderr into
# error records; so every measurement writes a bare filename in a scratch dir.
$scratch = [IO.Path]::Combine([IO.Path]::GetTempPath(), "hero-psnr-$PID")
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function Measure-Psnr {
    param([string]$A, [string]$B, [string]$FilterA = '', [string]$FilterB = '')
    $graph = "[0:v]${FilterA}format=rgb24[a];[1:v]${FilterB}format=rgb24[b];[a][b]psnr=stats_file=psnr.txt"
    Push-Location $scratch
    try {
        & ffmpeg -hide_banner -loglevel error -y -i $A -i $B -filter_complex $graph -f null - | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg could not measure $A against $B" }
    } finally { Pop-Location }
    $stats = Get-Content (Join-Path $scratch 'psnr.txt') -Raw
    if ($stats -notmatch 'psnr_avg:([0-9.]+|inf)') { throw "Unexpected psnr output: $stats" }
    if ($Matches[1] -eq 'inf') { return [double]::PositiveInfinity }
    return [math]::Round([double]$Matches[1], 2)
}

try {
    # 1. The uncommitted frames must be the frames the committed still was cut from.
    $record = Get-Content $stillRecord -Raw | ConvertFrom-Json
    $box = $record.crop
    $halves = @{ original = 20; neural = 740 }   # the still's layout: 20 px margin, 20 px gutter, 80 px label band
    foreach ($kind in @('original', 'neural')) {
        $fromFrame = "crop=$($box.width):$($box.height):$($box.x):$($box.y),"
        $fromStill = "crop=$($box.width):$($box.height):$($halves[$kind]):80,"
        $match = Measure-Psnr -A $inputs[$kind] -B $still -FilterA $fromFrame -FilterB $fromStill
        if (-not [double]::IsPositiveInfinity($match)) {
            throw "$($inputs[$kind]) is not the frame docs/media/stills/007-first-light-bond.png was cut from ($match dB, not identical). Refusing to build a hero from it."
        }
        Write-Host "  $kind frame matches the committed still exactly"
    }

    # 2. The crop and one encode.
    $window = "crop=$($crop.W):$($crop.H):$($crop.X):$($crop.Y),"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    foreach ($kind in @('original', 'neural')) {
        $out = Join-Path $outDir "hero-$kind.jpg"
        if ((Test-Path $out) -and -not $Force) { Write-Host "  exists hero-$kind.jpg (use -Force to regenerate)"; continue }
        & ffmpeg -hide_banner -loglevel error -y -i $inputs[$kind] `
            -vf "crop=$($crop.W):$($crop.H):$($crop.X):$($crop.Y)" -q:v $quality -frames:v 1 $out
        if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed on $($inputs[$kind])" }
        Write-Host "  wrote  hero-$kind.jpg"
    }

    # 3. The encode must not have made the pair more alike than the frames are.
    $plates = @{ original = Join-Path $outDir 'hero-original.jpg'; neural = Join-Path $outDir 'hero-neural.jpg' }
    $reference = Measure-Psnr -A $inputs.original -B $inputs.neural -FilterA $window -FilterB $window
    $pair = Measure-Psnr -A $plates.original -B $plates.neural
    Write-Host ''
    Write-Host "  PSNR between the plates: $pair dB (the lossless frames, same window: $reference dB)"
    if ($pair -gt $reference + $MaxPairGainDb) {
        throw "The encoded plates are $pair dB apart where the frames are $reference dB apart. The encode has smoothed the render's effect away and the hero would understate it."
    }
    Write-Host '  the plates keep the difference the frames carry.' -ForegroundColor Green

    $hash = { param($p) (Get-FileHash -Algorithm SHA256 -Path $p).Hash.ToLowerInvariant() }
    [ordered]@{
        generatedBy      = 'site/tools/make-hero-crops.ps1'
        sources          = [ordered]@{
            original = [ordered]@{ file = 'tools/demo-video/public/bond-1122-original.png'; sha256 = & $hash $inputs.original }
            neural   = [ordered]@{ file = 'tools/demo-video/public/bond-1122-neural.png'; sha256 = & $hash $inputs.neural }
            writtenBy = 'tools/demo-video/prepare-inputs.py'
            matchesStill = 'docs/media/stills/007-first-light-bond.png, both halves pixel for pixel'
        }
        provenance       = 'docs/media/stills/007-first-light-bond.provenance.json'
        crop             = "x=$($crop.X) y=$($crop.Y) w=$($crop.W) h=$($crop.H) in the 2560x1440 frame"
        operations       = "crop only, one JPEG encode at -q:v $quality; no scaling, colour adjustment or sharpening"
        cropBoxChosenBy  = 'the active (non-letterbox) picture, with the face across the page seam at 55% and the headline on the dark panelling'
        psnrBetweenCrops = $pair
        psnrBetweenFrameWindows = $reference
        note             = '007 First Light - Story Trailer (IO Interactive), source frame 1122 (0:18.719), and the same frame from the whole-video render at default neural settings: RTX 4080 SUPER, driver 610.47, runtime lock 310.8.SF-v2, player 0.25.0 plus main (dced888).'
    } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $outDir 'provenance.json') -Encoding utf8
    Write-Host '  wrote  provenance.json'
} finally {
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
