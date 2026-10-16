# Related DLSS 5 neural-rendering projects

_Verified against 0.28.3 (f42cb39) on 2026-10-16._

Every link, and every fact in "Where this player fits" and "The landscape", was
checked on the project's public page on 2026-10-16. Star counts are that day's,
rounded, and will drift.

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
not (see the table). The nearest are Lunarc, which keeps ranges you choose to
pre-render and marks them on its timeline, and DLSS5Tool, whose preview can
pre-render the frames that follow; neither renders the whole video while it
plays. Frame generation here is a separate conversion
(**DLSS > Generate frames** writes a new file); it is not part of live
playback.

## The landscape on 2026-10-16

**NVIDIA.** DLSS 5 shipped officially on 2026-09-03 with GeForce Game Ready
Driver 616.64, on RTX 50 Series GPUs only and in one game (NBA 2K27)
([NVIDIA's announcement](https://www.nvidia.com/en-us/geforce/news/nba-2k27-dlss-5-3d-guided-neural-rendering-geforce-game-ready-driver/)).
On 2026-10-13 NVIDIA announced a model twice as fast, again for RTX 50 Series
GPUs (and RTX Spark laptops and GeForce NOW), in NBA 2K27 from 2026-10-16 and
in RTX Remix from 2026-10-23
([NVIDIA](https://www.nvidia.com/en-us/geforce/news/dlss-5-faster-ai-model-hogwarts-legacy-rtx-spark-laptops/)).
Neither announcement names a public SDK. The latest public
[DLSS SDK](https://github.com/NVIDIA/DLSS) (310.9.1) and
[Streamline SDK](https://github.com/NVIDIA-RTX/Streamline) (2.14.1), both
released on 2026-09-08, add nothing for it: the first adds a Ray
Reconstruction preset, the second frame-generation pacing options.

| Project | What it is | Relevant to this player |
| --- | --- | --- |
| [Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) (Merserk, ~1,380★) | Image and video processor. [v16.0](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v16.0) (2026-10-14), shipped as a .7z. A reorderable pipeline of stages (neural pass, scaling, DLSS and RTX Video Super Resolution, RTX Video HDR, colour matching and LUTs, NIS or CAS sharpening, film grain), DLSS frame generation, 10-bit HDR, Split/2-Up/Output previews, ProRes HQ and FFV1 export, and a command-line tool. v16.0 adds video stages from NVIDIA's Video Effects SDK: Video Super Resolution with Denoise, Deblur and High-Bitrate modes, and Video Frame Generation with scene-cut detection. [v15.0](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v15.0) (2026-10-08) moved to the runtime's native controls (Style 0, 1 and 2, and an RGB control mask whose channels drive intensity, local tone and local structure), removed its own Face/Skin Protection, Shimmer Suppression, Automatic Mask and NR Color Strength controls, and lets an export pause and resume from its last committed part, releasing GPU memory while paused and pausing before Windows suspends | The most complete converter. v16.0 removed its Live workspace, so nothing plays through its pipeline any more; its video previews render a short range from the selected position and cache the result. Windows 11 only. Source-available under its own licence, not open source |
| [NeuralScreen](https://github.com/perseval-BLR/NeuralScreen) (~1,110★) | Whole-desktop overlay, real time, with a before/after wipe, recording and frame generation, plus file conversion. [v2.4.0](https://github.com/perseval-BLR/NeuralScreen/releases/tag/v2.4.0) (2026-10-11). Since v2.2.0 an edit stabiliser carries only the render's difference from the source along motion; v2.4.0 ties how long it remembers to the time between frames, after shading trailed on moving faces | The overlay applies the model to whatever is on screen, not to a video file, and nothing is rendered ahead or kept; file conversion is a separate offline step. PolyForm Strict licence |
| [Veyra](https://github.com/Likely7/Veyra-NRVideo) (Likely7, ~730★) | Real-time player for files, images, capture cards and game streams (PS5, Sunshine PC, Xbox). [2.0.8](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.0.8) (2026-10-15), in separate NVIDIA and AMD packages. Up to four stacked neural layers, DLSS Super Resolution, RTX Video Super Resolution or FSR, live frame generation up to 6× (DLSS-G, XeSS or FSR), a game mode that passes input through to the captured window, HDR input kept, and an export queue (MP4 or MKV) that keeps audio and subtitle tracks | Renders what is playing and does not keep it; an original/enhanced toggle rather than compare views. Mostly tested on an RTX 5070. Windows 11. GPL-3.0, with AGPL-3.0 for its streaming part |
| [DLSS5Tool](https://github.com/banbanzhige/DLSS5Tool) (banbanzhige, ~150★) | Image and video converter with a live preview. [v3.0.0](https://github.com/banbanzhige/DLSS5Tool/releases/tag/v3.0.0) (2026-10-11). RTX Video 2× and 4× Super Resolution, 2× to 4× frame interpolation, HDR10 and HLG 10-bit output, and optional RAFT or NVOFA optical flow. v3.0.0 adds live enhancement of a running window, presets with protected regions, and a preview that can pre-render the frames that follow | A converter with split, side-by-side and frame-by-frame comparison before export; its new live mode enhances a window, not a video file. GPL-3.0 since 2026-10-08, when its live module was added; the earlier MIT notice is kept for the original code |
| [Lunarc](https://github.com/Yukikaze1945/Lunarc-DLSS5) (Yukikaze1945, new) | Video and image enhancer with a live preview. [v1.2.2](https://github.com/Yukikaze1945/Lunarc-DLSS5/releases/tag/v1.2.2) (2026-10-15). One to three neural layers, DLAA, RTX VSR, NVIDIA Video Frame Generation for export, TrueHDR, NVOF, AMD or RAFT optical flow, and a manager that downloads, imports and switches DLSS-NR versions after checking their digests and signatures | Pre-renders a range you select to an encoded segment, keeps it across restarts while the source and settings still match, marks it with a green bar on the timeline and plays it beside the original. Export keeps audio, subtitles and chapters where the container allows. GPL-3.0-or-later |
| [DLSS5-Image-Converter](https://github.com/criso2hd-alt/DLSS5-Image-Converter) (criso2hd-alt, ~57★) | Image and video converter. v0.6.2 (2026-09-29). A wipe and a difference view, "Compare styles" (Natural and Cinematic side by side, optionally with the source), tiled Ultra Detail that runs the neural pass over overlapping tiles of a much larger image, depth from Depth Anything V2, H.264 video export with audio, and a 3D tab that turns an image into a Gaussian-splat scene | Stills first. It uses the RenoDX add-on, as this player does; its experimental OptiScaler backend is marked as not working yet. Source-available, not open source |
| [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr) (~180★) | CLI converter with a Gradio UI and ComfyUI nodes. v1.4.1 (2026-09-17). GPU-resident: decode, Super Resolution, optical flow, neural rendering and encode without a CPU round trip | A faster offline pipeline than this player's ffmpeg child. No playback or compare view for video. MIT |
| [dlss5-nr-player](https://github.com/Zonnery/dlss5-nr-player) | Real-time player with a live side-by-side view, plus an offline converter. RTX 50 only. No commits since 2026-08-31 | Renders what is playing and does not keep it. [A fork](https://github.com/scegielski/dlss5-nr-player) (v0.2.0, 2026-09-19) adds split and wipe views, RTX Video Super Resolution after the neural pass, multipass and RTX 40 support |
| [MPCVR-Plus](https://github.com/HumbleUser33/MPCVR-Plus) (HumbleUser33; was MPCVR-DLSS5) | A fork of MPC Video Renderer (DirectShow) that adds an optional neural pass to its Direct3D 11 pipeline, with a temporal stabiliser driven by NVIDIA Optical Flow, optional DLSS Super Resolution, and new upscalers and chroma filters. [Release 2.0](https://github.com/HumbleUser33/MPCVR-Plus/releases/tag/MPCVR-Plus-2.0) (2026-10-13) presents each picture for its own refresh, reports 19 ms a picture with the neural pass at 25 Hz where 1.9.1 took 39, and brings the neural pass back after an evaluation the GPU did not answer | Renders while you watch, a little ahead of the audio clock, and does not keep frames; no compare view is described. GPL-3.0 |
| [DLSSNR PotPlayer Plugin](https://github.com/222222222l/DLSSNR-Potplayer-Plugin) (222222222l) | An installer that routes PotPlayer x64 through MPC Video Renderer, ReShade, DLSS5-Feeder and RenoDX so the neural pass runs inside PotPlayer. v0.1 pre-release (2026-09-06), no commits since | Real time, nothing kept; settings are changed in the RenoDX overlay. The author notes the in-player controls are not yet verified end to end. No licence file |
| [openplayer-dlssnr](https://github.com/AreChen/openplayer-dlssnr) (AreChen) | A plugin for OpenPlayer 1.6.4 or later, built on DLSS5Tool's backend. v0.4.0 pre-release (2026-09-12). GPU choice, a processing frame-rate cap (15 fps by default), HDR and Dolby Vision converted to SDR first, and a paused-frame comparison while adjusting | Real time, SDR output only, up to 3840 × 2160; nothing kept. No licence file |
| [OBS-DLSS5-Neural-Filter](https://github.com/SAOG0721/OBS-DLSS5-Neural-Filter) (SAOG0721, new) | A source filter for OBS Studio on Direct3D 11: one to three neural passes, protection and detail controls, four anti-flicker modes, a 25% to 100% inference scale, and Normal, Original, Split and Difference views. Tagged v0.1.0 (2026-10-14), with no published release on 2026-10-16 | Filters one OBS source for streaming or recording; nothing kept. GPL-3.0 |
| [DaVinci-Resolve-DLSS5](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5) (SAOG0721, ~100★) | A same-resolution OpenFX filter for DaVinci Resolve. [v0.4.1](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5/releases/tag/v0.4.1) pre-release (2026-10-12). One to three neural passes, hue, shadow and highlight protection on the correction, SDR and HDR (PQ, HLG) input, optical flow or Fusion motion vectors, five anti-flicker modes (new in 0.4.1), an Output Mix control, and `Difference x10` and Split output views | Works inside an editor's timeline. MIT |
| [DLSS5-for-Nuke](https://github.com/KJzzzKJ/DLSS5-for-Nuke) (KJzzzKJ, ~48★) | A native node for Nuke 15 and 17, `DLSS5Live`, with a background worker. v1.0.0 (2026-09-04). The neural pass at 1:1, then optional DLSS Super Resolution; Single Frame, Sequence (temporal history with OpenCV DIS flow or external motion vectors) and an experimental CG Multi-pass mode with depth and mask inputs | A compositing node; no player. MIT. A copy under another account adds a prebuilt zip that is not in the original and points every README link at it; this page links the original |
| [ComfyUI-DLSS5-Enhancer](https://github.com/Blueforcer/ComfyUI-DLSS5-Enhancer) (~290★) | ComfyUI nodes for image batches and whole video files, with an automatic skin mask and optional upscaling. No commits since 2026-09-02 | A node-graph workflow; no player. MIT |
| [ComfyUI-DLSS5-NR](https://github.com/lisitskyaa/ComfyUI-DLSS5-NR) (~170★) | ComfyUI nodes (v0.3.1, 2026-09-10), still and temporal modes, the runtime's automatic mask path | Frames go through CPU staging on every upload and readback. MIT |
| [OptiScaler-DLSSNR-PreSR-Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) (~820★) | Game mod built on [OptiScaler](https://github.com/optiscaler/OptiScaler), which has moved to the `optiscaler` organisation and has no neural-rendering support of its own | [Issue #100](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/issues/100), open, reports its neural pass leaving the output of an offscreen DLAA harness like this one unchanged; DLSS5-Image-Converter reports the same of its OptiScaler backend. RenoDX's add-on remains the only runtime known to work here |
| [ctype-lab/dlss5-video-player](https://github.com/ctype-lab/dlss5-video-player) | A fork of this project | It no longer lists any releases; its last commit is from 2026-09-14 |

Re-uploads of these projects under other accounts have been reported carrying
malware ([Visual Enhancer #97](https://github.com/Merserk/dlss5-visual-enhancer/issues/97)
names a trojanised copy of a ComfyUI node pack). More have appeared since:
"mirror" repositories of a deleted project whose links lead to spam domains, a
release re-packed as a .7z under an account named after the original author,
and "…-Download" repositories made for search engines. The links on this page
go to the original repositories; download from those.

Game injectors such as [DLSS 5 Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot)
(~890★) and [DLSS5-Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) bring
the renderer to games rather than to video; they are listed below for the ideas
this player took from them.
[Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor)
(~180★, 0.3.1 on 2026-10-10) is a ReShade add-on that runs a game's neural pass
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
