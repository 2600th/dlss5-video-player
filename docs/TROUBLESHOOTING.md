# Troubleshooting

## Independent upscaling build

- Extract the whole package into a new folder. A root-level `dxgi.dll` is an old
  layout; the new neural DLLs belong only in `neural-runtime/`.
- DLSS Upscaling starts off. Open a video, then enable it from the DLSS menu or
  bottom bar. Choose 1440p/default or 2160p output. If the source meets/exceeds
  the output target, it stays native; no source detail is discarded.
- SR startup/evaluation failure keeps ordinary playback available. Check
  `DLSSVideoPlayer.log` beside the player for the NGX result.
- Neural-job diagnostics are beside `neural-runtime/NeuralWorker.exe`. Switching
  SR or the comparison view does not invalidate or re-encode a verified cache.

## Experimental neural mode is unstable

Use **Advanced > Restart in DLSS SR safe mode**. This disables the RenoDX add-on
for that launch while retaining native DLSS Super Resolution. A normal-mode
status label is configuration evidence only; inspect ReShade's Add-ons page for
observed add-on status.

## DLSS is not active

Check the player's NGX status and evaluation counter. Try `F6` once after the
video is open. Do not replace a single packaged DLL; the 310.8 runtime set is
locked as one configuration.

## YouTube playback fails

Only public non-DRM videos are supported. Private, login-required, paid,
age-gated, cookie-dependent, or DRM-protected videos are outside the resolver
contract. Availability and region access can change. Local playback remains
available when YouTube resolution is unavailable.

## YouTube video quality is too low

Choose **Video > YouTube source quality**. Auto requests the best available
stream; a fixed height falls back to the best stream below it. This setting is
separate from **DLSS > Mode / quality**.

## Playback drops frames

Use Auto, Balanced, or Performance quality at a lower output resolution. The
player preserves playback time by dropping stale frames rather than slowing the
video.

## ReShade captures input

Use `Ctrl+Alt+Space` for play/pause, `Ctrl+Alt+Left/Right` to seek,
`Ctrl+Alt+M` to mute, `Ctrl+Alt+D` to toggle DLSS, and `Ctrl+Alt+C` for image
adjustments.

## The surface flickers

Test safe mode. If flicker occurs only with the experimental add-on active,
record the GPU, driver, resolution, and observed ReShade add-on status when
reporting it.
