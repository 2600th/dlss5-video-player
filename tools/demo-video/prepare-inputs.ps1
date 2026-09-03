param([string]$RawCaptureDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$public = Join-Path $PSScriptRoot 'public'
New-Item -ItemType Directory -Force -Path $public | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'docs/screenshots/current/neural-playback.jpg') -Destination (Join-Path $public 'hero.jpg')
if ($RawCaptureDirectory) {
    # Original session recordings: retain their exact timing, no speed changes.
    & ffmpeg -hide_banner -loglevel error -ss 9 -i (Join-Path $RawCaptureDirectory 'face-raw.mp4') -t 9 -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'face-take.mp4')
    if ($LASTEXITCODE) { throw 'Face take extraction failed' }
    & ffmpeg -hide_banner -loglevel error -ss 1 -i (Join-Path $RawCaptureDirectory 'playback-on-raw.mp4') -t 15 -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'playback-take.mp4')
} else {
    # A clean checkout can recover both genuine recordings from the delivered
    # video. This adds a lossy generation; use the originals for best quality.
    $demo = Join-Path $root 'docs/media/neural-comparison-demo.mp4'
    & ffmpeg -hide_banner -loglevel error -ss 3 -i $demo -t 9 -vf 'crop=1442:932:427:75:exact=1' -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'face-take.mp4')
    if ($LASTEXITCODE) { throw 'Face take recovery failed' }
    & ffmpeg -hide_banner -loglevel error -ss 12 -i $demo -t 15 -vf 'crop=1442:932:427:75:exact=1' -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'playback-take.mp4')
}
if ($LASTEXITCODE) { throw 'Playback take extraction failed' }
