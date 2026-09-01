# Experimental neural-rendering mode

The 0.12.0 release ZIP already contains its exact locked runtime set. Extract the
whole ZIP and run `DLSSVideoPlayer.exe`; do not replace individual DLLs with files
from another pack.

On detected RTX 40 and RTX 50 GPUs, normal mode enables
`renodx-dlss5.addon64` by default. This was tested locally on an RTX 4080. RTX 50
is an intended best-effort target and was not hardware-tested here.

The bundled neural runtime is modified and reports Authenticode `HashMismatch`.
The ReShade proxy and RenoDX add-on are unsigned. These signature states do not
by themselves indicate malware or safety. See `EXPERIMENTAL_RUNTIME_NOTICE.txt`.

## Checking observed status

The player status reports the selected configuration, not proof that a neural
workload evaluated successfully. Press **Home**, open ReShade's **Add-ons** page,
and inspect the RenoDX add-on's observed status. Native NGX status and evaluation
counters are shown in the player.

## Safe mode

If the experimental path is unstable, choose **Advanced > Restart in DLSS SR
safe mode**. Safe mode disables only the RenoDX neural add-on for that launch and
keeps native DLSS Super Resolution available. A later normal launch on an RTX
40/50 policy target restores the default setting with at most one bootstrap
relaunch.
