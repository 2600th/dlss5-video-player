# Related DLSS 5 neural-rendering projects

_Verified against 0.26.2 (b965b53) on 2026-09-30._

Every link, and every fact in "Where this player fits" and "The landscape", was
checked on the project's public page on 2026-09-30. Star counts are that day's
and will drift.

## Where this player fits

This player is built to let you **check** what the neural renderer did. The
original frame and the render of that same frame sit behind one toggle, a
split, a wipe, a difference map or a 2 × 2 beside NVIDIA's RTX VSR, and
switching never moves the playhead.

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

## The landscape on 2026-09-30

**NVIDIA.** DLSS 5 shipped officially on 2026-09-03 with GeForce Game Ready
Driver 616.64, on RTX 50 Series GPUs only and in one game (NBA 2K27)
([NVIDIA's announcement](https://www.nvidia.com/en-us/geforce/news/nba-2k27-dlss-5-3d-guided-neural-rendering-geforce-game-ready-driver/)).
The announcement names no public SDK. The latest public
[DLSS SDK](https://github.com/NVIDIA/DLSS) (310.9.1) and
[Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) (2.14.1), both
released on 2026-09-08, add nothing for it: the first adds a Ray
Reconstruction preset, the second frame-generation pacing options.

| Project | What it is | Relevant to this player |
| --- | --- | --- |
| [Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) (Merserk, ~1,130★) | Image and video processor. v13.2 (2026-09-29). A reorderable pipeline of stages (neural pass, DLSS and RTX Video Super Resolution, RTX Video HDR, colour matching and LUTs, NIS or CAS sharpening, scaling from 25% to 200%), Face/Skin Protection, frame generation, 10-bit HDR, masks, shimmer suppression, a Split/2-Up preview, ProRes HQ and FFV1 export (since v11.0), a command-line tool (since v13.0), and a Live mode for local files, network streams, YouTube and Twitch | The most complete converter. Its Live mode shows frames as they are processed, from a 2 to 30 second buffer, and does not keep them; its video previews render and cache a chosen 1 to 30 second range for the Split/2-Up view. Source-available under its own licence, not open source |
| [NeuralScreen](https://github.com/perseval-BLR/NeuralScreen) (~990★) | Whole-desktop overlay, real time, with a before/after wipe, recording and frame generation, plus file conversion. v2.1.9 (2026-09-29). Since v2.1.4, conversion keeps a video's display matrix, a photo's EXIF orientation and its ICC profile | The overlay applies the model to whatever is on screen, not to a video file, and nothing is rendered ahead or kept; file conversion is a separate offline step. PolyForm Strict licence |
| [DLSS5Tool](https://github.com/banbanzhige/DLSS5Tool) (banbanzhige, ~112★) | Image and video converter. v2.3.3 (2026-09-24). Three styles, a queue that mixes images and videos with per-item settings, RTX Video 2× and 4× Super Resolution, 2× to 4× frame interpolation, HDR10 and HLG 10-bit output, and optional RAFT or NVOFA optical flow to steady the neural pass between frames. RTX 30, 40 and 50 | A converter with a live preview and split, side-by-side and frame-by-frame comparison before export; not a player. The project's own code is MIT |
| [DLSS5-Image-Converter](https://github.com/criso2hd-alt/DLSS5-Image-Converter) (criso2hd-alt, ~53★) | Image and video converter. v0.6.2 (2026-09-29). A wipe and a difference view, "Compare styles" (Natural and Cinematic side by side, optionally with the source), tiled Ultra Detail that runs the neural pass over overlapping tiles of a much larger image, depth from Depth Anything V2, H.264 video export with audio, and a 3D tab that turns an image into a Gaussian-splat scene | Stills first. It uses the RenoDX add-on, as this player does; its experimental OptiScaler backend is marked as not working yet. Source-available, not open source |
| [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr) (~176★) | CLI converter with a Gradio UI and ComfyUI nodes. v1.4.1 (2026-09-17). GPU-resident: decode, Super Resolution, optical flow, neural rendering and encode without a CPU round trip | A faster offline pipeline than this player's ffmpeg child. No playback or compare view for video. MIT |
| [dlss5-nr-player](https://github.com/Zonnery/dlss5-nr-player) | Real-time player with a live side-by-side view, plus an offline converter. RTX 50 only. No commits since 2026-08-31 | Renders what is playing and does not keep it. [A fork](https://github.com/scegielski/dlss5-nr-player) (v0.2.0, 2026-09-19) adds split and wipe views, RTX Video Super Resolution after the neural pass, multipass and RTX 40 support |
| [MPCVR-DLSS5](https://github.com/HumbleUser33/MPCVR-DLSS5) (HumbleUser33) | A fork of MPC Video Renderer (DirectShow) that adds an optional neural pass to its Direct3D 11 pipeline, with a temporal stabiliser driven by NVIDIA Optical Flow, optional DLSS Super Resolution, and new upscalers and chroma filters. Release 1.5 (2026-09-29) | Renders while you watch, a little ahead of the audio clock, and does not keep frames; no compare view is described. GPL-3.0 |
| [DLSSNR PotPlayer Plugin](https://github.com/222222222l/DLSSNR-Potplayer-Plugin) (222222222l) | An installer that routes PotPlayer x64 through MPC Video Renderer, ReShade, DLSS5-Feeder and RenoDX so the neural pass runs inside PotPlayer. v0.1 pre-release (2026-09-06), no commits since | Real time, nothing kept; settings are changed in the RenoDX overlay. The author notes the in-player controls are not yet verified end to end. No licence file |
| [openplayer-dlssnr](https://github.com/AreChen/openplayer-dlssnr) (AreChen) | A plugin for OpenPlayer 1.6.4 or later, built on DLSS5Tool's backend. v0.4.0 pre-release (2026-09-12). GPU choice, a processing frame-rate cap (15 fps by default), HDR and Dolby Vision converted to SDR first, and a paused-frame comparison while adjusting | Real time, SDR output only, up to 3840 × 2160; nothing kept. No licence file |
| [DaVinci-Resolve-DLSS5](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5) (SAOG0721, ~94★) | A same-resolution OpenFX filter for DaVinci Resolve. v0.3.1 pre-release (2026-08-31). An Output Mix control, and `Difference x10` and `Left / Right Compare` output views | Works inside an editor's timeline. Frames go through CPU copies each way and motion and depth are zero, so the author calls its preview speed unsuitable for production. MIT |
| [DLSS5-for-Nuke](https://github.com/KJzzzKJ/DLSS5-for-Nuke) (KJzzzKJ, ~46★) | A native node for Nuke 15 and 17, `DLSS5Live`, with a background worker. v1.0.0 (2026-09-04). The neural pass at 1:1, then optional DLSS Super Resolution; Single Frame, Sequence (temporal history with OpenCV DIS flow or external motion vectors) and an experimental CG Multi-pass mode with depth and mask inputs | A compositing node; no player. MIT. A copy under the 2148-wq account adds a prebuilt zip that is not in the original and points every README link at it; this page links the original |
| [ComfyUI-DLSS5-Enhancer](https://github.com/Blueforcer/ComfyUI-DLSS5-Enhancer) (~264★) | ComfyUI nodes for image batches and whole video files, with an automatic skin mask and optional upscaling. No commits since 2026-09-02 | A node-graph workflow; no player. MIT |
| [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR) (~162★) | ComfyUI nodes (v0.3.1, 2026-09-10), still and temporal modes, the runtime's automatic mask path | Frames go through CPU staging on every upload and readback. MIT |
| [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) (~746★) | Game mod built on OptiScaler | [Issue #100](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/100), open, reports its neural pass leaving the output of an offscreen DLAA harness like this one unchanged; DLSS5-Image-Converter reports the same of its OptiScaler backend. RenoDX's add-on remains the only runtime known to work here |
| [ctype-lab/dlss5-video-player](https://github.com/ctype-lab/dlss5-video-player) | A fork of this project | It no longer lists any releases; its last commit is from 2026-09-14 |

Game injectors such as [DLSS 5 Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot)
(~797★) and [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) bring
the renderer to games rather than to video; they are listed below for the ideas
this player took from them.
[Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor)
(~166★, 0.2.5 on 2026-09-21) is a ReShade add-on that runs a game's neural pass
on a second GPU while the first renders the game, with measurements on two
RTX 5060 Ti cards. MIT.

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
