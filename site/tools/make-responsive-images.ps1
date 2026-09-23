#Requires -Version 5.1
<#
.SYNOPSIS
    Encodes the AVIF and WebP variants the page serves in place of full-size
    JPEGs: the hero pair, and each gallery scene's pair.

.DESCRIPTION
    A phone was loading both 1920x1080 hero JPEGs (295 KB and 255 KB) because
    the page offered nothing smaller. This writes each plate at a few widths in
    AVIF and WebP, which the page offers through <picture> and srcset. The JPEGs
    stay as the fallback, and the committed full-size captures stay what the
    gallery's 1:1 loupe shows, so real pixels are always one click away.

    Both plates of a pair go through identical parameters: the same area
    downscale (averaging, no sharpening kernel) and the same encoder settings.
    An encode that smoothed the render's effect away would make the comparison
    flatter the product, so the script measures, per width and per format, the
    PSNR between the two encoded plates against the PSNR between the two
    sources scaled the same way, losslessly. If encoding has made the pair more
    alike by more than $MaxPairGainDb, it refuses. It also records how close
    each file is to its own source, so a later reader can see what was traded.

    Requires ffmpeg built with libaom-av1 and libwebp. Run from anywhere; paths
    resolve against the repository. Existing files are kept unless -Force.
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [double]$MaxPairGainDb = 0.5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$assets = [IO.Path]::Combine($repoRoot, 'site', 'src', 'assets')
$shots = [IO.Path]::Combine($repoRoot, 'docs', 'screenshots', 'current')

# AVIF keeps 4:4:4 and full range, tagged as the JPEG's own BT.601 matrix with
# sRGB primaries and transfer, so a browser decodes it back to the same RGB.
# WebP lossy is 4:2:0 by definition. The qualities were chosen by measurement:
# about 40 dB against the source for both formats at the hero's widths.
$avifArgs = @('-c:v', 'libaom-av1', '-still-picture', '1', '-crf', '30', '-cpu-used', '4',
    '-color_range', 'pc', '-colorspace', 'bt470bg', '-color_primaries', 'bt709', '-color_trc', 'iec61966-2-1')
$webpArgs = @('-c:v', 'libwebp', '-quality', '82', '-compression_level', '6')
$scaleFlags = 'area+accurate_rnd'

$sets = @(
    @{
        Name = 'hero'; OutDir = Join-Path $assets 'hero'; Widths = @(960, 1280, 1920)
        Original = Join-Path $assets 'hero\hero-original.jpg'
        Neural = Join-Path $assets 'hero\hero-neural.jpg'
        Stem = 'hero'
    }
    @{
        Name = 'lucia'; OutDir = Join-Path $assets 'gallery'; Widths = @(960, 1440)
        Original = Join-Path $shots 'gta6-lucia-original.jpg'
        Neural = Join-Path $shots 'gta6-lucia-neural.jpg'
        Stem = 'lucia'
    }
    @{
        Name = 'matrix'; OutDir = Join-Path $assets 'gallery'; Widths = @(960, 1440)
        Original = Join-Path $shots 'matrix-original.jpg'
        Neural = Join-Path $shots 'matrix-neural.jpg'
        Stem = 'matrix'
    }
    @{
        Name = 'gta-player'; OutDir = Join-Path $assets 'gallery'; Widths = @(960, 1440)
        Original = Join-Path $shots 'original-comparison.jpg'
        Neural = Join-Path $shots 'neural-playback.jpg'
        Stem = 'gta-player'
    }
)

function Invoke-Ffmpeg {
    param([string[]]$Arguments)
    & ffmpeg -hide_banner -loglevel error -y @Arguments
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg failed: $($Arguments -join ' ')" }
}

# PSNR in RGB between two inputs, each run through its own filter prefix. The
# stats go to a bare filename in a scratch directory: ffmpeg's filter parser
# reads the colon of a Windows drive path as an option separator, and Windows
# PowerShell turns a native command's stderr into terminating error records.
$scratch = [IO.Path]::Combine([IO.Path]::GetTempPath(), "responsive-psnr-$PID")
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function Measure-Psnr {
    param([string]$A, [string]$B, [string]$FilterA = '', [string]$FilterB = '')
    $graph = "[0:v]${FilterA}format=rgb24[a];[1:v]${FilterB}format=rgb24[b];[a][b]psnr=stats_file=psnr.txt"
    Push-Location $scratch
    try { Invoke-Ffmpeg @('-i', $A, '-i', $B, '-filter_complex', $graph, '-f', 'null', '-') }
    finally { Pop-Location }
    $stats = Get-Content (Join-Path $scratch 'psnr.txt') -Raw
    if ($stats -notmatch 'psnr_avg:([0-9.]+|inf)') { throw "Unexpected psnr output: $stats" }
    if ($Matches[1] -eq 'inf') { return [double]::PositiveInfinity }
    return [math]::Round([double]$Matches[1], 2)
}

$records = @{}
try {
    foreach ($set in $sets) {
        New-Item -ItemType Directory -Force -Path $set.OutDir | Out-Null
        foreach ($source in @($set.Original, $set.Neural)) {
            if (-not (Test-Path $source)) { throw "Source missing: $source" }
        }

        $entries = @()
        foreach ($width in $set.Widths) {
            $scale = "scale=${width}:-2:flags=$scaleFlags,"
            $reference = Measure-Psnr -A $set.Original -B $set.Neural -FilterA $scale -FilterB $scale

            foreach ($format in @('avif', 'webp')) {
                $files = @{}
                foreach ($kind in @('original', 'neural')) {
                    $source = if ($kind -eq 'original') { $set.Original } else { $set.Neural }
                    $name = "$($set.Stem)-$kind-$width.$format"
                    $out = Join-Path $set.OutDir $name
                    if ((Test-Path $out) -and -not $Force) {
                        Write-Host "  exists $name"
                    } else {
                        if ($format -eq 'avif') {
                            Invoke-Ffmpeg (@('-i', $source, '-vf', "scale=${width}:-2:flags=${scaleFlags}:out_range=full,format=yuv444p") + $avifArgs + @('-frames:v', '1', $out))
                        } else {
                            Invoke-Ffmpeg (@('-i', $source, '-vf', "scale=${width}:-2:flags=$scaleFlags") + $webpArgs + @('-frames:v', '1', $out))
                        }
                        Write-Host "  wrote  $name"
                    }
                    $files[$kind] = @{
                        file = $name
                        bytes = (Get-Item $out).Length
                        psnrAgainstSource = Measure-Psnr -A $out -B $source -FilterB $scale
                    }
                }

                $pair = Measure-Psnr -A (Join-Path $set.OutDir $files.original.file) -B (Join-Path $set.OutDir $files.neural.file)
                if ($pair -gt $reference + $MaxPairGainDb) {
                    throw ("{0} {1} at {2}px: the encoded pair is {3} dB apart where the sources are {4} dB apart. The encode has smoothed the render's effect away; refusing to ship it." -f $set.Name, $format, $width, $pair, $reference)
                }
                $entries += [ordered]@{
                    width = $width
                    format = $format
                    pairPsnr = $pair
                    sourcePairPsnr = $reference
                    original = $files.original
                    neural = $files.neural
                }
                Write-Host ("  {0} {1} {2}px: pair {3} dB (sources {4} dB)" -f $set.Name, $format, $width, $pair, $reference)
            }
        }

        if (-not $records.ContainsKey($set.OutDir)) { $records[$set.OutDir] = [ordered]@{} }
        $records[$set.OutDir][$set.Name] = [ordered]@{
            original = $set.Original.Substring($repoRoot.Length + 1).Replace('\', '/')
            neural = $set.Neural.Substring($repoRoot.Length + 1).Replace('\', '/')
            variants = $entries
        }
    }
} finally {
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}

# Provenance beside the files, so a later reader knows what each one is and
# what the encode cost. PSNR is measured in RGB against the source scaled the
# same way, and pairPsnr against sourcePairPsnr is the check above.
foreach ($dir in $records.Keys) {
    [ordered]@{
        generatedBy = 'site/tools/make-responsive-images.ps1'
        operations = "area downscale ($scaleFlags), then one encode; no sharpening or colour adjustment"
        avif = ($avifArgs -join ' ')
        webp = ($webpArgs -join ' ')
        provenance = 'docs/screenshots/README.md'
        sets = $records[$dir]
    } | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $dir 'variants.json') -Encoding utf8
    Write-Host "  wrote  $(Split-Path $dir -Leaf)/variants.json"
}
