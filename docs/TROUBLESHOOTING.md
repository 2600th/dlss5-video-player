# Troubleshooting

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
