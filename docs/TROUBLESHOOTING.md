# Troubleshooting

## The player opens but DLSS is not active

Check the title/status line. A working native NGX path should eventually show `NGX CREATE OK`, an increasing evaluation counter and a successful raw result (`0x1`).

Try `F6` after ReShade and the RenoDX add-on have fully initialized.

## RenoDX / DLSS 5 stays in standby

Confirm that:

- ReShade was installed with add-on support.
- `renodx-dlss5.addon64` is beside the executable.
- `nvngx_dlssnr.dll` is beside the executable.
- Matching DLSS/DLSSNR runtime files are used together.
- The add-on is enabled in ReShade's **Add-ons** page.

Run `prepare_dlss5_test.bat` and inspect `DLSSVideoPlayer.log` plus `ReShade.log`.

The player status describes the configured mode, not a successful neural evaluation:

- `Neural addon enabled (experimental)` means the RTX 40/50 startup policy left the RenoDX add-on enabled.
- `DLSS SR safe mode` means `--safe-mode` disabled only `renodx-dlss5.addon64`; native signed DLSS Super Resolution remains available.

Use **Advanced > Restart in DLSS SR safe mode** if the experimental path is unstable. A required `ReShade.ini` correction relaunches the player at most once before renderer creation. If `ReShade.ini` is malformed or read-only, startup fails closed and leaves the file unchanged; the exact reason is written to `DLSSVideoPlayer.log`. RTX 50 enablement is covered by policy tests only and has not been hardware-verified on this project's RTX 4080 SUPER development system.

## Video playback is slower than the source

Use **Auto** DLSS mode first. The player is designed to preserve source time and drop late frames when necessary. The status line shows:

```text
fps rendered/source | drop N
```

High `drop` counts indicate the current DLSS/Neural Rendering workload is too expensive for the selected output/input combination.

Try Balanced or Performance mode for high-frame-rate 4K material.

## The ReShade overlay is open and player controls do not respond

Use the overlay-safe hotkeys:

```text
Ctrl+Alt+Space   Play / Pause
Ctrl+Alt+Left    Back 10 s
Ctrl+Alt+Right   Forward 10 s
Ctrl+Alt+M       Mute
Ctrl+Alt+D       DLSS on/off
Ctrl+Alt+C       Image adjustments
```

## The video surface flashes when moving the mouse

Current builds use a no-background render child window, `WS_CLIPCHILDREN`/`WS_CLIPSIBLINGS`, limited UI invalidation and explicit `WM_ERASEBKGND`/`WM_PAINT` handling so GDI should no longer erase the D3D12 surface during mouse movement.

If flashing still occurs, test once with ReShade disabled. If it only occurs with ReShade, include the ReShade version and log in a bug report.

## Seek crashes or hangs

Seeking is transactional: playback is paused, audio is stopped/joined, GPU work is synchronized, the decoder is repositioned/reopened if needed, temporal state is reset, then playback resumes.

If a file still crashes on seek, attach `DLSSVideoPlayer.log` and include the container/codec information from `ffprobe`.

## A video does not appear in the Open dialog

The first file-picker filter is **All files (FFmpeg auto-detect)**, so the dialog should not hide uncommon extensions. You can also drag the file directly onto the player.

## No audio

Verify `ffmpeg.exe` exists beside the player. Some files can contain audio formats or channel layouts that the current PCM helper path cannot open. Include the `ffprobe` stream information when reporting the problem.
