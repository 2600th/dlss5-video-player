# Related DLSS 5 neural-rendering projects

_Verified against 0.25.0 (8afb573) on 2026-09-23._

Every link, and every fact in "Where this player fits" and "The landscape", was
checked on the project's public page on 2026-09-23. Star counts are that day's
and will drift.

## Where this player fits

This player is built to let you **check** what the neural renderer did. The
original frame and the render of that same frame sit behind one toggle, a
split, a wipe or a blend, and switching never moves the playhead.

Among the projects below, it is the only one we found that does all three of
these at once:

1. renders the **whole video progressively** in the background, nearest to
   the playhead first, while you watch;
2. **keeps every rendered frame**, so a seek lands on rendered frames and a
   finished render is reused the next time the video is opened;
3. shows the **original and the render on the same frame while it is still
   rendering**.

Several others do one or two of these, and many do things this player does
not (see the table). Frame generation here is a separate conversion
(**DLSS > Generate frames** writes a new file); it is not part of live
playback.

## The landscape on 2026-09-23

**NVIDIA.** DLSS 5 shipped officially on 2026-09-03 with GeForce Game Ready
Driver 616.64, on RTX 50 Series GPUs only and in one game (NBA 2K27)
([NVIDIA's announcement](https://www.nvidia.com/en-us/geforce/news/nba-2k27-dlss-5-3d-guided-neural-rendering-geforce-game-ready-driver/)).
The announcement names no public SDK.

| Project | What it is | Relevant to this player |
| --- | --- | --- |
| [Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) (Merserk, ~1,000★) | Image and video processor. v11.0 (2026-09-21). Frame generation, 10-bit HDR, RTX Video Super Resolution, masks, shimmer suppression, a Split/2-Up preview, ProRes and FFV1 export, and a Live mode | The most complete converter. Its Live mode shows frames as they are processed and does not keep them; its 2-Up view is part of the preview workflow. Source-available under its own licence, not open source |
| [NeuralScreen](https://github.com/perseval-BLR/NeuralScreen) (~945★) | Whole-desktop overlay, real time, with a before/after wipe, recording and frame generation | Applies the model to whatever is on screen, not to a video file; nothing is rendered ahead or kept. PolyForm Strict licence |
| [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr) (~171★) | CLI converter with a Gradio UI and ComfyUI nodes. GPU-resident: decode, Super Resolution, optical flow, neural rendering and encode without a CPU round trip | A faster offline pipeline than this player's ffmpeg child. No playback or compare view for video. MIT |
| [dlss5-nr-player](https://github.com/Zonnery/dlss5-nr-player) | Real-time player with a live side-by-side view, plus an offline converter. RTX 50 only | Renders what is playing and does not keep it. [A fork](https://github.com/scegielski/dlss5-nr-player) adds split and wipe views, RTX Video Super Resolution after the neural pass, multipass and RTX 40 support |
| [ComfyUI-DLSS5-Enhancer](https://github.com/Blueforcer/ComfyUI-DLSS5-Enhancer) (~230★) | ComfyUI nodes for image batches and whole video files, with an automatic skin mask and optional upscaling | A node-graph workflow; no player. MIT |
| [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR) (~162★) | ComfyUI nodes (v0.3.1), still and temporal modes, the runtime's automatic mask path | Frames go through CPU staging on every upload and readback. MIT |
| [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) (~681★) | Game mod built on OptiScaler | [Issue #100](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/100), open, reports its neural pass leaving the output of an offscreen DLAA harness like this one unchanged. RenoDX's add-on remains the only runtime known to work here |
| [ctype-lab/dlss5-video-player](https://github.com/ctype-lab/dlss5-video-player) | A fork of this project | Its latest release is v0.21.2 (2026-09-12) |

Game injectors such as [DLSS 5 Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot)
(~742★) and [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) bring
the renderer to games rather than to video; they are listed below for the ideas
this player took from them.

## Adopted ideas

From a review on 2026-09-01; each still describes where an idea here came
from.

- [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) documents the
  RenoDX `[RenoDX.DLSS5]` controls. This player adapted its MIT-licensed
  configuration contract: use its raw-NGX-only hook mode (`EnableHooks=2`),
  enable neural uplift, explicitly disable RenoDX upscaling, and preserve
  unrelated user settings. The Streamline hook mode is unnecessary because
  this player calls NGX directly.
- [DLSS 5 Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot) treats the
  feeder route as native DLAA and warns against mixing independently versioned
  runtime components. This project likewise defaults to DLAA and hash-locks an
  atomic runtime set.
- [Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer), then at
  v2.1, recorded exact component fingerprints and required feature-18 evidence before accepting offline output. This project hash-locks
  runtime inputs and keeps configured state separate from successful NGX
  evaluation evidence.
- [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR) confirms a
  persistent feature-18 lifetime, native 1:1 output, driver-store NGX discovery,
  and caller-validation shim mechanics. Its direct bridge is a useful reference
  for a future official or runtime-authorized backend.

## Deliberately not copied

This project keeps its persistent GPU resource ring and the RenoDX inline
interception path. It does not combine a direct feature-18 caller shim with the
RenoDX add-on, because doing so risks two neural evaluations for one output and
two undocumented NGX lifetimes. CPU staging per frame, as in
ComfyUI-DLSS5-NR, suits a tensor workflow but would add latency and bandwidth
to a player that already owns the D3D12 resources.

Only the MIT-licensed setting names and policy were adapted; no renderer or
bridge implementation was copied. No proprietary runtime or game file from any
related project is committed here.

Source licenses apply independently. The DLSS5-Feeder MIT notice used by this
project is retained under `THIRD_PARTY_LICENSES/`.
