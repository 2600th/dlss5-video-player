# Experimental DLSS 5 Neural Rendering setup

Native DLSS Super Resolution is integrated directly through NVIDIA NGX. Experimental DLSS 5 Neural Rendering is a separate runtime path that currently relies on ReShade and the RenoDX DLSS 5 add-on.

## Required external files

Download the DLSS 5 runtime package and `renodx-dlss5.addon64` from:

https://app.mediafire.com/folder/sa9zioqbixj7e

The repository and normal GitHub release package intentionally do not redistribute those experimental files.

Place the required files in the same folder as `DLSSVideoPlayer.exe`. At minimum, the experimental setup expects:

```text
nvngx_dlssnr.dll
renodx-dlss5.addon64
```

Use the matching `nvngx_dlss.dll` supplied by the same package when one is provided. Do not mix DLLs from unrelated package versions unless you are deliberately testing compatibility.

## ReShade

Install a ReShade build with **add-on support** for `DLSSVideoPlayer.exe` and select the DirectX 10/11/12 renderer option.

First-run note: I don't remember whether the current ReShade build enables third-party add-ons automatically. Press **Home**, open the **Add-ons** page and confirm that the RenoDX DLSS 5 add-on is enabled.

## Runtime verification

Run:

```bat
prepare_dlss5_test.bat
```

The checker reports whether the expected DLL/add-on files are present and can invoke `inspect_dlssnr.ps1` for version/hash/signature information.

Then launch:

```bat
run_dlss5_test.bat
```

Inside the player, verify:

- `NGX CREATE OK` appears in the title/status area.
- `evalC` increases while video is playing.
- The raw NGX result is `0x1` on successful native DLSS evaluation.
- Motion-vector, depth and mask debug views are non-empty and react to video content.

Use `F6` to recreate the NGX feature if the add-on loaded after initial NGX setup.

## ReShade overlay controls

Normal player input may be captured by ReShade while its overlay is open. These Windows-level hotkeys remain available:

```text
Ctrl+Alt+Space   Play / Pause
Ctrl+Alt+Left    Back 10 s
Ctrl+Alt+Right   Forward 10 s
Ctrl+Alt+M       Mute
Ctrl+Alt+D       DLSS on/off
Ctrl+Alt+C       Image adjustments
```

The player keeps presenting the frozen frame while paused so ReShade remains visually responsive during comparisons.
