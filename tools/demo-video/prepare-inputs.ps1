param([string]$RawCaptureDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$public = Join-Path $PSScriptRoot 'public'
New-Item -ItemType Directory -Force -Path $public | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'docs/screenshots/current/neural-playback.jpg') -Destination (Join-Path $public 'hero.jpg')
Copy-Item -LiteralPath (Join-Path $root 'docs/screenshots/current/godfather-neural.jpg') -Destination (Join-Path $public 'hero-godfather.jpg')
if ($RawCaptureDirectory) {
    # Original session recordings: retain their exact timing, no speed changes.
    & ffmpeg -hide_banner -loglevel error -i (Join-Path $RawCaptureDirectory 'godfather-toggle.mp4') -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'face-take.mp4')
    if ($LASTEXITCODE) { throw 'Toggle take copy failed' }
    & ffmpeg -hide_banner -loglevel error -i (Join-Path $RawCaptureDirectory 'gta6-play.mp4') -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'playback-take.mp4')
} else {
    # A clean checkout can recover both genuine recordings from the delivered
    # video: the player window sits at a fixed place in the frame, and the two
    # takes occupy 3-13 s and 13-28 s. This adds one lossy generation; use the
    # originals for best quality.
    $demo = Join-Path $root 'docs/media/neural-comparison-demo.mp4'
    & ffmpeg -hide_banner -loglevel error -ss 3 -i $demo -t 10 -vf 'crop=1442:932:426:74:exact=1' -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'face-take.mp4')
    if ($LASTEXITCODE) { throw 'Toggle take recovery failed' }
    & ffmpeg -hide_banner -loglevel error -ss 13 -i $demo -t 15 -vf 'crop=1442:932:426:74:exact=1' -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public 'playback-take.mp4')
}
if ($LASTEXITCODE) { throw 'Playback take extraction failed' }
