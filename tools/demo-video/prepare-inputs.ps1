# Stages the four player recordings that `scenes` in `src/index.tsx` loads into
# `public/`. Every take is a raw screen recording of the shipping player window
# that the operator supplies; this script only names it, limits it to the length
# the cut consumes and re-encodes it. It never trims the head: each scene enters
# its recording at its own `trim` second through `trimBefore`, and the measured
# `zoom` / `wipe` instants are offsets into the raw recording, so second 0 of the
# file has to stay second 0 of the take.
#
# -RawCaptureDirectory must therefore hold these four recordings, under these
# names, each at least as long as the composition consumes (`trim` + `length`):
#
#   playback-godfather.mp4  6.8 s  The Godfather playing with the render attached
#   compare-godfather.mp4   6.8 s  that frame paused, then 'Z' and Video > Compare > Wipe
#   compare-gta6.mp4        6.7 s  the same two controls on a paused GTA VI frame
#   playback.mp4            4.9 s  uninterrupted GTA VI playback, neural view left on
#
# There is no clean-checkout fallback from `docs/media/neural-comparison-demo.mp4`,
# because none is possible: that file is the finished edit, so it contains no frame
# before any scene's `trim`, and over its two comparison scenes the ORIGINAL /
# NEURAL RENDERED chips are already composited inside the window the crop would
# take, which the composition would then draw a second time.
param([Parameter(Mandatory)][string]$RawCaptureDirectory)
$ErrorActionPreference = 'Stop'
$public = Join-Path $PSScriptRoot 'public'
New-Item -ItemType Directory -Force -Path $public | Out-Null
# Asset name, then the seconds the cut uses: the scene's `trim` plus its `length`.
$takes = @(
    @{Name = 'playback-godfather.mp4'; Seconds = '6.8'},
    @{Name = 'compare-godfather.mp4'; Seconds = '6.8'},
    @{Name = 'compare-gta6.mp4'; Seconds = '6.7'},
    @{Name = 'playback.mp4'; Seconds = '4.9'}
)
foreach ($take in $takes) {
    # Original session recordings: retain their exact timing, no speed changes.
    & ffmpeg -hide_banner -loglevel error -i (Join-Path $RawCaptureDirectory $take.Name) -t $take.Seconds -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -an -y (Join-Path $public $take.Name)
    if ($LASTEXITCODE) { throw "Staging $($take.Name) failed" }
}
