# DLSS 5 Video Player — AMD

**Put any video through DLSS 5's neural renderer on an AMD Radeon, and check
every frame against the original.**

A free, open-source player for Windows. Open a video, a photo or a GIF and press
`D`: within seconds the render plays while the rest of the video renders behind
you, and the original is one key away on the same frame. Other DLSS 5 tools
convert files or filter the desktop live; this one renders the whole video
progressively, keeps every frame, and shows you what the model changed, and where
it didn't help. [How it compares](docs/RELATED_PROJECTS.md).

> [!NOTE]
> **A fork of [DLSS 5 Video Player](https://github.com/2600th/dlss5-video-player)
> with an AMD backend.** The player, the compare views, the render cache, the
> export pipeline and the documentation are upstream's work. What this fork adds
> is a backend for all three stages: the neural pass runs on an RDNA 4 Radeon
> through `LmxxfNrRuntime`, and upscaling and frame generation on FidelityFX's
> engines, with no NVIDIA add-on, no NGX and no ReShade. **The NVIDIA backend is
> upstream's and is not supported here** — see the note further down.
> [What the AMD backend does, and does not do](#on-an-amd-card) ·
> [AMD captures](#what-it-looks-like) ·
> [integration plan and status, in Chinese](docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md) ·
> [the runtime package](third_party/lmxxf/README.md)

[Upstream project](https://github.com/2600th/dlss5-video-player) · [On an AMD card](#on-an-amd-card) · [Build it](#building-and-contributing) · [First run](#first-run) · [Usage guide](docs/USAGE.md) · [Troubleshooting](docs/TROUBLESHOOTING.md)

> [!IMPORTANT]
> Community project, not an NVIDIA or an AMD product. The neural runtime on
> either backend is a modified, unsigned community build. ~~On the NVIDIA backend
> neural rendering needs an RTX card and driver 610.47 or newer;~~ on the AMD
> backend it needs an RDNA 4 Radeon and the runtime package, which this
> repository does not ship — see [On an AMD card](#on-an-amd-card). Notices:
> [THIRD_PARTY.md](THIRD_PARTY.md).

## On an AMD card

This fork's addition. Neural rendering runs through `LmxxfNrRuntime.dll` — the
AMD runtime — instead of the NVIDIA add-on, and upscaling and frame generation
have engines of their own behind the same menu items. The player picks the
backend itself at startup: if the NVIDIA layout is absent and the AMD runtime
package is present and matches the manifests it ships, neural rendering is
offered on it. It is not a menu choice, because a machine has one of the two.
`--neural-backend amd` does the same from the command line.

| | AMD backend |
| --- | --- |
| GPUs | RDNA 4 (`gfx1200`, `gfx1201`); measured on an RX 9070 XT |
| Stages it can run | All three: neural rendering (`nr`), upscaling (`sr`) and frame generation (`fg`). Not NVIDIA's engines: no RTX VSR, no DLSS Super Resolution, no DLSS-G — this backend brings its own |
| Upscaler | FSR 4 (FidelityFX upscaler 4.1.1), else FSR 1's EASU, else Catmull-Rom, the last two sharpened with CAS. One setting, **DLSS > Upscaled by**, serves the source frames and the interpolated ones alike |
| Frame generation | FidelityFX frame generation: live at 2x during playback (**Frame Generation during playback**, `Ctrl+Alt+G`, off by default), or a 2x–5x offline conversion (**DLSS > Generate frames**) that writes a new file |
| Input size | The runtime admits up to 1920×1080 by pixel budget. A 1440p or 4K source has to be scaled down before it reaches the runtime |
| Processing scale | Must be 100%. The runtime returns the frame at the size it was given, so a reduction would be silently ignored; this player refuses instead. The upscaler runs after the model, on the frame it returned |
| Passes | 1 to 4, stacked, with switches for the later ones only: keep skin tones, predict the third pass instead of running the network, and skip the later passes' block group. Every extra pass is another full network run |
| Temporal model | The neural pass has none: no guide estimator, so its motion and depth guides are constants and the render is spatial. FSR 4 is a temporal upscaler, but it is handed constants too — no jitter, depth, exposure or motion vectors — so it upsamples the frame as given |
| Colour | SDR. 10-bit and HDR are not wired on this path |
| Speed | ≈21 ms a frame end-to-end at 1080p on an RX 9070 XT for the neural pass alone, against a 33.3 ms budget at 30 fps. The upscale's and the interpolation's own cost is unmeasured, and so are 900, 1440 and 4K |
| What it needs | The `amd-neural-runtime/` package (≈600 MiB, not in this repository) beside the module directory, or named by `DLSS5_AMD_RUNTIME_DIR`. For the two newer stages, FidelityFX's `amd_fidelityfx_loader_dx12.dll` beside the exe, with `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` — three files, in the packaged build, that [the notices](docs/THIRD-PARTY-NOTICES.md) list |
| Add-on, ReShade, `dxgi.dll` | None. This backend loads no proxy and registers no add-on |

Measured on an RX 9070 XT: the player resolves and logs the pack at startup, the
offline export writes a file, a range render with preroll — the shape a live
session uses — passes, a 744-frame clip converts to 1,488 frames (30 → 60 fps)
with its audio and its duration, and the upscaler takes a 1280x720 source to
2560x1440 through the tier it starts on. Generated frames were also measured
frame by frame against the two frames they sit between, which is how the
brightness strobe in the interpolated cadence was found and then fixed. The
captures below are a live session's compare views, so `D` has since been driven
end to end on the same card; **a cache hit after a session and half-hour
continuous playback are still unverified.** The
[integration plan](docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md) (Chinese) records
what was measured, on what, and what was not.

### Upscaling and frame generation

The two stages that used to be NVIDIA's alone run here too, on AMD's FidelityFX
engines and the player's own shaders rather than on NVIDIA's.

**Upscaling.** The toolbar's DLSS Upscaling switch is answered by this backend,
not by NGX: the pill names the tier serving the frame — `DLSS Upscaling · On ·
FSR1` — and **DLSS > Upscaled by** picks it: Auto, FSR 4, FSR1 or CAS. Auto is
the cascade FSR 4, then FSR 1's EASU, then Catmull-Rom, with CAS sharpening on
the last two. A chosen tier is the cascade's entry point, not a dead end: one
that fails hands the frame to the next tier down and says so in the log, so the
two halves of a cadence can never end up on different engines. FSR 4 is AMD's
learned, temporal upscaler and needs the FidelityFX upscaler DLL; without it
EASU serves every frame, and without that, Catmull-Rom. The output height is
**Upscaling output** — 1080p, 1440p, 2160p, or Auto for the largest your monitor
shows — and the upscale happens after the model, on the frame it returned: a
1280x720 source still renders at 1280x720 and is then presented at 2560x1440. In
an export, the Super Resolution stage writes with the same stretch-and-sharpen
that is the playback chain's last resort, Catmull-Rom then CAS; FSR 4 is the
playback present, not the export's writer.

**Frame generation.** Two different things, both FidelityFX's frame generation.
**Frame Generation during playback** (`Ctrl+Alt+G`, the **Interpolate** pill)
interpolates on screen as the video plays, at 2x the source rate: off by
default, remembered, and the frame-rate chip shows the higher rate only while
interpolated frames are actually being presented. **DLSS > Generate frames** is
the offline conversion — a new file at 2x to 5x, 2x by default. Three rules hold
either way. An interpolated frame takes the same upscaler tier as the real
frames around it, so a doubled cadence does not alternate in sharpness. It is
clamped into the brightness envelope of its two neighbours, so a hard edge
cannot overshoot. And its colour is decoded once, by the sampler — the doubled
sRGB decode that used to make generated frames come out bright is gone.

### What it looks like

![Bumblebee in Split view on the AMD backend: the source left of a hard divider down the face, the render right of it, both tagged](docs/media/amd/amd-split-view.png)

One frame of *Bumblebee* (2018) in **Split**, on the AMD backend: source left,
render right, the same frame on both sides. All five captures here are of that
film rather than of the game trailers above: film grain, a shallow depth of
field, and faces lit for a camera rather than for a renderer.

[![Bumblebee in Wipe, animated: the source left of a divider through the face, the render right of it, the divider sweeping across](docs/media/amd/amd-live-wipe-preview.webp)](docs/media/amd/amd-live-wipe.mp4)

Six seconds of the film in **Wipe**, while it plays: the divider sweeps across
the face, so both halves are the same frame at every moment. The animation above
is the clip, scaled and silent — [the MP4 keeps its sound](docs/media/amd/amd-live-wipe.mp4).

![2 x 2 on the AMD backend, a Bumblebee frame: original, DLSS 5, Difference at 4x in luma, and DLSS 5 at Mix 50%, each tagged](docs/media/amd/amd-2x2.png)

**2 × 2**: original, DLSS 5, **Difference** (4x, luma) and DLSS 5 at
**Mix 50%**. The fourth pane is the second Mix on this backend, not RTX VSR:
that pane is NVIDIA's upscaler, and this machine has none to put beside the
render.

| Source | Render |
| --- | --- |
| ![A face from Bumblebee, the source frame](docs/media/amd/amd-face-source.png) | ![The same face, the AMD render](docs/media/amd/amd-face-render.png) |

A face, source left and render right, cropped from the film: pores, lashes and
the catchlight in the iris come back on the right.

The NVIDIA path is untouched by any of this, and upstream still develops it. It
is **not** supported in this fork: it needs an RTX card with driver 610.47 or
newer, which this fork neither ships nor verifies, and the 10-bit ladder, HDR and
NVIDIA's own engines — RTX VSR, DLSS Super Resolution, DLSS-G — come with it. On
the AMD backend the picture is SDR and 8-bit, and the upscaler and frame
generation are the ones [above](#upscaling-and-frame-generation). Everything
downstream that needs NVIDIA's engines is struck through and marked unsupported.

> [!WARNING]
> **NVIDIA-only features are not supported in this fork.** RTX VSR, DLSS Super
> Resolution and DLSS-G are NVIDIA engines and need an RTX card with driver
> 610.47 or newer; this fork ships no NVIDIA build and cannot verify one.
> Everything that claims or shows them below is struck through or labelled as
> unsupported — the demo, the screenshots, the RTX GPU table, the `R` and
> `Shift+R` views — but the material is kept, because it is upstream's and
> describes the same player. Where they used to be, this build has its own: FSR
> 4, FSR 1 or CAS for the upscale, FidelityFX frame generation for the frames
> between. [On an AMD card](#on-an-amd-card) is the place to start.

## Upstream demo and stills

*NVIDIA backend — not supported in this fork. Kept because the player and its
compare views are the same on either backend; what each backend can run is the
part that differs.*

[![25-second DLSS 5 Video Player demonstration: a 007 First Light face split between the source and the player's render, GTA VI playing with the divider, then the player's Difference, Side by side and loupe views, and a render filling the timeline while the video plays](docs/media/neural-comparison-preview.webp)](docs/media/neural-comparison-demo.mp4)

**[Watch the 25-second video](docs/media/neural-comparison-demo.mp4)** (1080p, no
sound), captured on the NVIDIA backend: *007 First Light* and *GTA VI* Trailer 2
rendered at default settings on an RTX 4080 SUPER, then the Difference, Side by
side and loupe views — the same compare views this build shows, on a backend this
build does not support.

![007 First Light, one frame: the source on the left, the same frame from the player's render at default settings on the right, identical unscaled 700x880 crops](docs/media/stills/007-first-light-bond.png)

One source frame (left) and the same frame of the player's render (right),
unscaled, on the NVIDIA backend. [Four more, including one where the model makes
the picture worse](docs/media/README.md#comparison-stills).

## Download

**This fork has no releases, and a source zip of it would not run on its own.**
The AMD runtime package is 600 MiB and is not in this repository, so there is no
backend in the source tree — the same reason upstream's "Source code" zip won't
run. Build it: [Build and test](docs/BUILDING.md). For the AMD backend, put the
`amd-neural-runtime/` package beside the module directory
([what is in it](third_party/lmxxf/README.md)).

Upstream publishes signed builds of the NVIDIA version on
[its releases page](https://github.com/2600th/dlss5-video-player/releases), with
a `.sha256` beside each zip, GitHub attestations of what that repository built,
and a `verify_package.ps1` that checks the unpacked files against the package
manifest. Those are the upstream project's, not this fork's, none of them has an
AMD backend, and this fork does not support that backend — they are listed so a
reader knows where the other one lives.

## First run

1. Keep `neural-runtime/` next to the exe. (Upstream's zips say to unpack into a
   new, empty folder first.) On the AMD backend the `amd-neural-runtime/`
   package has to be reachable as well, and the three FidelityFX DLLs — loader,
   upscaler, frame generation — beside the exe for the upscale and the
   interpolation: see [On an AMD card](#on-an-amd-card).
2. Run `DLSSVideoPlayer.exe`. The player is not code-signed, so Windows may
   say "Windows protected your PC": choose **More info > Run anyway**. This fork
   has no signed builds to check against, so if you did not build it yourself,
   know where the binary came from.
3. Open a file (`Ctrl+O`), paste a public YouTube link (`Ctrl+L`), or pick
   something from **File > Game trailers**.
4. Press `D`. It buffers for a few seconds, then plays the render.
5. The compare bar under the picture switches between DLSS 5, Original, Split,
   Wipe, Difference, Side by side and 2 × 2.
6. **DLSS > Convert & export** saves the rendered video to a file.

**File > Recent videos** reopens a video with its render, if the render covers
the whole video. Press `D` at the start to get one.

## What it does

- **Renders while you watch.** Playback moves onto rendered frames a few
  seconds after you press `D`, while the rest of the video fills in behind you,
  nearest first.
- **Seeks anywhere.** Rendered frames play wherever they are; elsewhere the
  original plays while the render catches up.
- **Shows you what the model did.** Split, Wipe, **Difference** (the change,
  amplified), **Side by side** and **2 × 2** views, a Mix slider, zoom to 8x, a
  4x **loupe**, a mask that limits DLSS 5 to part of the frame, and
  press-and-hold for the original. Switching views never moves the playhead.
- ~~**Compares against NVIDIA's own upscaler.** `R` shows RTX Video Super
  Resolution of the original as a view of its own and in the 2 × 2; `Shift+R`
  compares the other views against it. In the complete download, or a build
  made with the RTX Video SDK.~~ **Not supported in this fork** — that view
  needs an NVIDIA card. The AMD backend's own upscaler is under [On an AMD
  card](#upscaling-and-frame-generation).
- **Saves the evidence.** **File > Save comparison image** writes exactly what
  the picture shows, with a footer recording the video, frame, view, neural
  settings and runtime.
- **Subtitles and HDR.** SRT, ASS, WebVTT, PGS and VobSub are drawn over the
  render, so the model never warps the text. HDR10 and HLG are tone mapped to
  SDR for the model; ~~on an HDR display the original still shows in HDR.~~ On
  the AMD backend the output stays SDR, so the HDR display path is not available
  there.
- **Keeps your renders.** A whole-video render is reused while the source,
  runtime and settings still match, at Standard, High (10-bit) or Lossless
  quality. **Advanced > Render report** shows the flicker, grain and colour
  shift a render added.
- **Works on screenshots and GIFs.** A photo renders once and stays on its
  frame; an animated GIF renders frame by frame, keeping its timing. See
  [Screenshots and GIFs too](#screenshots-and-gifs-too).
- **Saves the result as a file.** MP4 or MKV for a video, GIF for an
  animation, PNG or JPEG for a photo, from the player or the command line. See
  [Save the result](#save-the-result).
- **Tunes the model.** Change a neural setting (`Ctrl+N`) while paused and that
  frame re-renders. ~~**DLSS > Processing scale** runs the model at 75% or 50%
  of the source, for speed. Where the window shows a video larger than it is,
  NVIDIA's RTX Video Super Resolution upscales it, on by default: it measured
  well above a plain scaler and DLSS Super Resolution on video
  ([measurement](docs/measurements/vsr-quality-20261002/REPORT.md)). DLSS Super
  Resolution can still upscale playback to 1080p, 1440p or 2160p; it is off by
  default (see [Limits](#limits)).~~ **Not supported in this fork.** The AMD
  backend runs the model at the source size, so its processing scale has to stay
  at 100%, and it upscales with FSR instead — one switch and one menu in the same
  places ([details](#upscaling-and-frame-generation)).
- **Plays like a player.** AC-3, E-AC-3 and DTS passthrough to a receiver, and
  Windows media controls with taskbar thumbnail buttons.
- **Comes with test material.** Seven official game trailers, chosen for faces,
  skin and light, under **File > Game trailers** and on the start screen.

## Screenshots and GIFs too

![A Mafia: The Old Country trailer frame saved as a PNG and opened in the player as a photo, in Wipe: the original left of a divider down the man's nose, the DLSS 5 render right of it](docs/screenshots/current/photo-wipe.jpg)

Open a PNG, JPEG, BMP, TIFF or static WebP and press `D`. It renders once, the
compare views work as they do on a video, and it saves back as a PNG or JPEG
at full size. Above, a *Mafia: The Old Country* trailer frame saved as a
2560x1440 PNG, at default settings: the game's smooth orange skin comes back
with texture, a truer tone and light that falls across the face. ~~From the
command line it took 8 seconds on an RTX 4080 SUPER.~~ Animated GIFs render
frame by frame and keep their timing.

## Save the result

Everything you can watch rendered, you can keep as a file.

| Source | Saved as |
| --- | --- |
| Video, local or YouTube | MKV (default) or MP4. MKV keeps the source audio, subtitles and chapters without re-encoding them. |
| Animated GIF | GIF (default), MP4 or MKV |
| Photo: PNG, JPEG, BMP, TIFF or static WebP | PNG (default) or JPEG, at the source size |

Two ways under **DLSS > Convert & export**, and one from the command line:

- **Save converted video** writes the render you already have, without
  rendering again. It needs a render of the whole video, or of the clip you
  marked and converted with `Ctrl+R`.
- **Export with DLSS stages** (`Ctrl+S`) renders a new file with any of Super
  Resolution, neural rendering and frame generation (2x to 5x the frame rate),
  in NVIDIA's order. ~~Super Resolution on its own runs on RTX VSR by default -
  the upscaler that measured best - or on DLSS Super Resolution if you choose
  it under **Upscaler**.~~ On the AMD backend all three stages run too; there the
  Super Resolution stage writes with the player's own stretch-and-sharpen,
  Catmull-Rom then CAS, and frame generation at 2x to 5x runs on FidelityFX's
  runtime.
- **From the command line**, `dlss5-convert` runs the same export without
  opening the player, waits for it and shows its progress. Give it files,
  folders or wildcards; this writes `clip-dlss.mkv` beside the input:

  ```bat
  dlss5-convert clip.mp4
  ```

  Every stage and Neural setting is an option. Three stacked neural passes of
  the strong look, frame generation at 3x, a folder upscaled to 4K
  ~~with RTX VSR (the default upscaler; `--sr-engine dlss` picks DLSS)~~ with
  whatever upscaler the machine has — on AMD, FSR 4, FSR 1 or CAS — and a folder
  rendered and upscaled together:

  ```bat
  dlss5-convert clip.mp4 --preset strong --passes 3
  dlss5-convert clip.mp4 --stages fg --multiplier 3
  dlss5-convert clips -r --stages sr --height 2160 --out-dir clips-4k
  dlss5-convert clips -r --stages sr,nr --height 2160 --out-dir clips-nr-4k
  ```

  `dlss5-convert probe clip.mp4` says what a file is and which stages this
  machine can run on it. `--range`, `--encode`, `--report` and the rest are in
  the [usage guide](docs/USAGE.md#from-the-command-line).

  On the AMD backend all three stages are the same switches; `--neural-backend
  amd` names that backend, and its Super Resolution and frame generation run on
  FidelityFX's engines under the same flags:

  ```bat
  dlss5-convert clips -r --stages sr,nr,fg --neural-backend amd
  ```

![Export with DLSS stages: Super Resolution ticked, output height 2160p, Upscaler RTX VSR (recommended), neural rendering and frame generation unticked, and the result line 3840 × 2160 at 30 fps, 1 pass](docs/screenshots/current/export-stages-vsr.jpg)

The dialog is the same on either backend; the **Upscaler** row in this capture is
NVIDIA's, and that choice is not supported here — on AMD the Super Resolution
stage writes with this build's own FSR upscaler.

## How it compares

Several open-source projects put DLSS 5's neural pass on video. This one is
built to watch the result and check it: it renders the whole video while you
watch, keeps the render, and shows the original beside it on the same frame.
As of 2026-10-02, from each project's README, releases and issues (details in
[RELATED_PROJECTS.md](docs/RELATED_PROJECTS.md); corrections welcome):

| | **DLSS 5 Video Player** (this) | [Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) v14.0 | [NeuralScreen](https://github.com/perseval-BLR/NeuralScreen) v2.1.9 | [Veyra](https://github.com/Likely7/Veyra-NRVideo) 2.0.0 | [DLSS5Tool](https://github.com/banbanzhige/DLSS5Tool) v2.3.3 |
| --- | --- | --- | --- | --- | --- |
| What it is | A player that renders the whole video in the background, plus export | A converter with a stage pipeline, plus a Live mode | An overlay on the whole desktop, plus file conversion | A real-time player for files, capture cards and game streams | A converter with a preview |
| Neural render you can keep, seek and re-watch | Yes: rendered frames are cached and reused | No: Live keeps a 2–30 s buffer | No: real time | No: real time | Export only |
| Compare tools | Split, Wipe, Difference, Side by side, 2×2, loupe, hold for original, against RTX VSR | Split, 2-Up | Before/after wipe | Original/enhanced toggle | Wipe, side by side |
| Upscaling | ~~RTX VSR by default in playback and export, DLSS SR on request; [measured](docs/measurements/vsr-quality-20261002/REPORT.md)~~ NVIDIA only, not supported in this fork. On AMD: FSR 4, FSR 1 or CAS | RTX VSR and DLSS SR stages, off by default | None (by design) | DLSS SR, RTX VSR or FSR | RTX VSR 2× or 4× |
| Frame generation | ~~DLSS-G 2–5× as a conversion; RTX 20/30 through an offered, hash-checked add-on~~ NVIDIA only, not supported in this fork. On AMD: FidelityFX 2–5× as a conversion, 2× live | DLSS-G as a conversion | DLSS-G 2–4×, live | DLSS-G, XeSS or FSR up to 6×, live | DLSS-G 2×, export |
| Inputs | Local video, public YouTube, photos, GIFs | Images, video, URLs, YouTube, Twitch | Anything on screen, files | Files, images, capture cards, PS5, PC and Xbox streams | Images, video, image sequences |
| Keeps audio / subtitles / chapters on export | Yes / yes / yes | Yes / no / yes | Audio yes | Audio and subtitles yes | Audio yes |
| HDR | HDR shown on HDR displays; exports are SDR | 10-bit HDR export, SDR→HDR | HDR10 recording (experimental capture) | HDR kept, HEVC Main10 export | HDR10/HLG export |
| Command line | `dlss5-convert`: files, folders, JSON report | `VE_CLI.exe` | No; a GUI queue | Not documented | Not documented |
| Download | None in this fork; upstream's is 377 MB (36 MB without the neural runtime) | 726 MB | 230 MB | 409 MB | 580 MB lite |
| Verifying the download | Upstream's: SHA-256, GitHub attestations, a file manifest and a verifier script | GitHub's per-file digest | SHA256SUMS and a runtime manifest | SHA-256 files | Runtime hashes |
| Licence | MIT | Source-available, its own terms | PolyForm Strict (non-commercial) | GPL-3.0 (AGPL for its streaming part) | MIT |

> Every row above describes the player on the **NVIDIA backend**, which is what
> upstream built and what this comparison was written against. The AMD backend
> runs the same three stages on different engines: FSR 4, FSR 1 or CAS for
> upscaling, FidelityFX frame generation at 2x live and 2x to 5x in a
> conversion, and the neural pass through `LmxxfNrRuntime` — see
> [On an AMD card](#on-an-amd-card).

Of the five, only this player keeps the render, so a seek lands on rendered
frames and a video you come back to is already done. It compares the render
with the original on the same frame in more ways than any of the others
~~, RTX VSR included~~; only its export keeps subtitles and chapters as well as
audio; and only its downloads carry GitHub attestations of what this repository
built. On the AMD backend the upscaler is this build's own; the measurement
linked above is upstream's, of NVIDIA's engine, and does not apply here.

None of these projects is an NVIDIA product, and each runs the neural pass on a
runtime build NVIDIA has not published for this use, this one included. Upstream
publishes its own builds only on
[its releases page](https://github.com/2600th/dlss5-video-player/releases);
re-uploads of DLSS 5 tools under other accounts have been reported carrying
malware. This fork publishes nothing — see [Download](#download).

## What's new in 0.28.2

Upstream's release notes for the player this fork is built on. They are about
the NVIDIA backend, which this fork does not support; the fork itself has no
release and no version of its own yet, and its changes are the AMD backend above,
in no numbered release.

**0.28.2** (2026-10-04). Neural settings now do what they say, and say what they
do. Renders made with the default settings are reused.

- **Apply changes the picture during neural playback** ([#15](https://github.com/2600th/dlss5-video-player/issues/15)).
  It used to restart the render and put the previous one straight back on
  screen. It now renders from the playhead with the new settings.
- **Intensity and Color strength reach every neural pass.** With two or more
  passes they changed only the first.
- **Every control says what it was measured to do**
  ([report](https://github.com/2600th/dlss5-video-player/blob/main/docs/measurements/knobs-upper-20261003/REPORT.md)).
  Intensity runs 0-1, because the runtime renders anything higher exactly as
  1.00; Style is the strongest control and costs no render time; Skin
  structure's "Off" was "As structure".
- **Neural settings opens with the Look preset** and a line that says what
  Apply will do to the picture on screen. The default preset is now
  **Standard**, and **Strong** really is stronger: 22 % further from the source
  than Standard, against 6 % before.
- **`dlss5-convert --style`**, so a script can render with one style and render
  the result again with another.
- Smaller fixes: a dead menu path in the frame generation prompt, measured
  ranges listed as keys on the shortcuts sheet, and a shorter status line.

Earlier releases are in [CHANGELOG.md](CHANGELOG.md); 0.28.1 made RTX VSR the
upscaler in playback and exports, 0.27.2 offered the
dlssg_sm86 add-on for Frame Generation on RTX 20 and 30, 0.27.0 added
`dlss5-convert`, the command-line converter, 0.26.2 fixed GPU decoding of
untagged HD video, and 0.26.0 added the compare views, RTX VSR beside DLSS 5,
subtitles, HDR, the render quality ladder and `--render`.

## Controls

| What | Key |
| --- | --- |
| Open a file / a YouTube URL | `Ctrl+O` / `Ctrl+L` |
| Play or pause | `Space` |
| Neural rendering on or off | `D` |
| Compare views | `C` / `Shift+C` step the mode; `X` swaps sides; `[` and `]` change the Mix |
| RTX VSR view; compare against it | ~~`R`; `Shift+R` (NVIDIA backend)~~ — not supported |
| Zoom, loupe | `Z` / `Shift+Z` zoom in and out at the pointer; `L` loupe |
| Save the view as a PNG | `Ctrl+Shift+S` |
| Subtitles; earlier, later | `V`; `H`, `J` |
| Every shortcut | `?` or `F1` |
| Seek ten seconds / step one frame | `Left` / `Right`; `.` |
| Mark In / Out; clear; go to time | `I` / `O`; `Shift+I`; `Ctrl+G` |
| Render one frame / four seconds / the marked clip | `F` / `Shift+F` / `Ctrl+R` |
| Generate frames; cancel a conversion | **DLSS > Generate frames**; `Esc` |
| Frame generation during playback | `Ctrl+Alt+G` (the **Interpolate** pill) |
| Neural settings | `Ctrl+N` |
| Export with DLSS stages | `Ctrl+S` |
| Image adjustments | `Ctrl+E` |
| Volume, mute | Mouse wheel; `M` |
| Fit or fill; fullscreen | `A`; `F11` |
| Stop | `S` |

Settings persist between launches. A fresh install starts with neural rendering
on, ~~RTX VSR Upscaling on,~~ DLSS Upscaling off, frame generation during
playback off, and DLSS's output and YouTube quality on **Auto**: the
largest resolution your monitor can show, and the best YouTube stream up to
1440p. RTX VSR is NVIDIA's and does nothing here; on the AMD backend the DLSS
Upscaling switch is answered by this player's own FSR upscale instead. Both are
covered under [On an AMD card](#on-an-amd-card).

The [usage guide](docs/USAGE.md) covers the rest, including frame generation
and the cache. The trailer list is in [EXAMPLE_VIDEOS.md](docs/EXAMPLE_VIDEOS.md).

## Screenshots (NVIDIA backend — not supported)

~~The player paused on one frame of *007 First Light* rendered at default
settings: the first four on an RTX 5090 at 100% display scaling, the next three
on an RTX 4080 SUPER at 150%.~~ **Not supported in this fork.** These are
upstream's captures, kept because the player and its compare views are the same
on either backend; this build's own captures are in [On an AMD
card](#what-it-looks-like). Click any image for full size.

![A 960x540 copy of 007 First Light in Wipe, compared against RTX VSR: the player's plain scale of the original left of a divider down the face, RTX VSR at High right of it, both tags at the top; an unscaled crop of the player's picture](docs/screenshots/current/vsr-wipe-crop.png)

One 540p frame shown at 1.51x: the player's plain scaling left of the divider,
RTX VSR right, unscaled crop ([whole window](docs/screenshots/current/vsr-wipe.jpg)).
The measured comparison is the
[RTX VSR quality report](docs/measurements/vsr-quality-20261002/REPORT.md), not
this frame.

![The player in Wipe: the original left of a divider down the face, the DLSS 5 render right of it, with the compare bar below](docs/screenshots/current/compare-wipe.jpg)

![Difference view: where the model changed the picture, amplified 4x, as brightness](docs/screenshots/current/compare-difference.jpg)

![2 x 2 view: original, DLSS 5, Difference and RTX VSR at High, with the toast confirming a saved comparison image](docs/screenshots/current/compare-2x2-toast.jpg)

The file that save wrote, footer and all:
[`saved-comparison-2x2.png`](docs/screenshots/current/saved-comparison-2x2.png).

![A subtitle drawn over the DLSS 5 frame (a test file that says what it is)](docs/screenshots/current/subtitles.jpg)

![Image adjustments, with the DLSS 5 mix slider, over the DLSS 5 frame](docs/screenshots/current/neural-strength.jpg)

![The Neural settings dialog at its defaults](docs/screenshots/current/neural-settings.jpg)

### Start screen

![Start screen on an RTX 5090: the capability check with its RTX VSR line, the Open file and Open YouTube URL actions, Recent tiles for GTA VI Trailer 2 and 007 First Light each marked Rendered 100%, and the other game trailers with their thumbnails](docs/screenshots/current/player-start.jpg)

### Neural view on and off

![GTA VI Trailer 2 paused on frame 1940 with the neural view on, on an RTX 5090](docs/screenshots/current/neural-playback.jpg)

![The same paused GTA VI frame with neural rendering off](docs/screenshots/current/original-comparison.jpg)

One paused frame with only the view switched, at default settings. It shows
how the toggle works, not what every source will gain.

[Capture details and footage attribution](docs/screenshots/README.md).

## Limits

- **Speed.** ~~On an RTX 5090 a live session costs about 6.8 ms a frame at
  1080p30, 9.9 ms at 1440p30 and 23 ms at 4K30, all faster than the video plays.
  An RTX 4080 SUPER measured 15.3 ms at 1080p30 on 0.16.0 and has not been
  re-measured since. 4K depends on the file.~~ Those are NVIDIA numbers, and this
  fork does not support that backend. The AMD figures are in [On an AMD
  card](#on-an-amd-card). After your first session the player knows your GPU and
  warns you before a video it expects to fall behind on.
- ~~**Driver 610.47 or newer (NVIDIA backend).** Older drivers refuse neural
  rendering on any card, and the player tells you up front. See
  [troubleshooting](docs/TROUBLESHOOTING.md#neural-rendering-is-refused-because-the-driver-is-too-old).~~
  **Not supported in this fork.** The AMD backend does not go through the NVIDIA
  driver's neural stack at all.
- ~~**Which RTX GPU does what (NVIDIA backend):**~~ **Not supported in this
  fork.**

  | ~~GPU~~ | ~~Neural rendering~~ | ~~RTX VSR upscaling~~ | ~~DLSS Super Resolution~~ | ~~Frame generation~~ |
  | --- | --- | --- | --- | --- |
  | ~~RTX 50~~ | ~~Yes; tested on an RTX 5090~~ | ~~Yes; measured on an RTX 5090~~ | ~~Yes~~ | ~~Yes, up to 5x here~~ |
  | ~~RTX 40~~ | ~~Yes; tested on an RTX 4080 SUPER~~ | ~~Yes~~ | ~~Yes~~ | ~~Yes, 2x~~ |
  | ~~RTX 30~~ | ~~Untested, and expected several times slower without native FP8~~ | ~~Yes~~ | ~~Yes~~ | ~~Only with the community dlssg_sm86 add-on; a user ran 4x on an RTX 3060~~ |
  | ~~RTX 20~~ | ~~As RTX 30, untested~~ | ~~Yes~~ | ~~Yes~~ | ~~Only with the add-on; untested~~ |

  ~~NVIDIA's own DLSS 5 support is RTX 50 only, and this player runs neural
  rendering on a modified community runtime on every generation, RTX 50
  included. RTX VSR costs about 1.5 ms for a 1080p frame
  and 2.7 ms for a 1440p one at High on an RTX 5090; slower cards pay more, and
  the player lowers its quality for a video it cannot keep up with.~~ **None of
  that applies to this fork.** For the AMD backend, [On an AMD
  card](#on-an-amd-card) has its own table, and the rest of this list holds for
  it unless it says otherwise.
- **Depth is estimated from the picture**, and so is motion on cards without
  the optical flow engine. Expect some artifacts.
- ~~**DLSS Super Resolution doesn't beat a plain scaler on video.** Against
  bicubic on six clips, it scored lower on all of them. It is built for games;
  use it for its look, not for detail. RTX VSR, the default upscaler, is the
  one that adds detail: 87.5 VMAF against bicubic's 80.5 and DLSS's 74.9 over
  the same kind of test.~~ **Both engines are NVIDIA's; not supported here.**
- **Frame generation writes a new file.** It takes time and disk space, and a
  stream has to be copied locally first. ~~On the NVIDIA backend RTX 40 and 50
  cards support it; on RTX 20 and 30 the player offers to download the
  community [dlssg_sm86 add-on](docs/TROUBLESHOOTING.md#rtx-20-and-30-the-dlssg_sm86-add-on),
  which is unofficial and not bundled.~~ On the AMD backend it runs on FidelityFX's
  frame generation at 2x to 5x; an export without that DLL falls back to the
  player's own block matcher, whose motion search is single-scale, so fast motion
  ghosts. Interpolating during playback needs the runtime, and is the cheap way
  to see the effect: nothing is written, and it is the same engine.
- **An upscale is an upscale, whichever engine writes it.** On the AMD backend
  FSR 4 is fed constants where a game would feed jitter, depth and motion
  vectors, so it behaves like a spatial upscaler with a learned filter, and its
  temporal history can only be as good as a decoded frame allows. FSR 1 and CAS
  are pure spatial filters. None of the three invents detail the neural pass did
  not produce — they present that frame larger, with CAS adding edge acutance on
  the way.
- **The two new stages cost time the neural pass did not.** On this backend both
  are extra passes over the same frame, so a machine already near its frame
  budget at 1080p will feel them; the figures in [On an AMD
  card](#on-an-amd-card) measure the neural pass alone.
- **Save converted video copies the cached render as it is**: 8-bit at the
  default Standard quality, 10-bit at High or Lossless, without adjustments,
  upscaling or HDR. Use **Export with DLSS stages** to bake in a larger size.
- **Not supported:** burning subtitles into an export, a render queue in the
  player (`dlss5-convert` converts a list or a folder in turn), or resuming an
  interrupted render after a restart.
- **YouTube:** public, non-DRM videos only, no login. Age-restricted videos can
  arrive as a 640x360 stream, and the status line tells you when that happens.
- **Disk space.** The cache keeps renders until the drive falls below 20 GB
  free, then deletes the least recently used first. **Advanced > Clear Neural
  Cache** shows how much it will free before it does.
- **Partial renders aren't joined.** A session started partway into a video
  leaves separate renders of the parts it covered, so reopening that video
  renders it again.
- **Network use.** Besides YouTube links, the player checks GitHub once a day
  for a new release (the menu bar then shows `↑ Update <version>`), and the
  start screen fetches the trailers' thumbnails from `i.ytimg.com`, again only
  when a cached one is a month old. `[Updates] Enabled=0` and
  `[Start] ThumbnailFetch=0` in `DLSSVideoPlayer.ini` turn these off.
  ~~On an RTX 20 or 30 card, the dlssg_sm86 add-on is downloaded only if you
  accept the offer.~~ That add-on is NVIDIA's and is not supported here.

## Building and contributing

C++20, Win32, Direct3D 12, FFmpeg, and on top of that one of two neural
runtimes: NVIDIA NGX on the NVIDIA backend, or `LmxxfNrRuntime` on the AMD one —
where FidelityFX's loader, upscaler and frame generation DLLs carry the other two
stages.

- [Build and test](docs/BUILDING.md), [runtime setup](docs/DLSS5_SETUP.md)
- [Architecture](docs/ARCHITECTURE.md), [technical overview](TECHNICAL_OVERVIEW.md)
- The AMD backend: [integration plan and status](docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md) (Chinese), and [the runtime package](third_party/lmxxf/README.md)
- Hardware test records: the `VERIFICATION-*.md` files in [docs](docs/) — upstream's, on the NVIDIA backend
- [Contributing](CONTRIBUTING.md): the checks a pull request must pass

**Reporting a bug:** open an [issue](https://github.com/myth2loki/dlss5-video-player-on-amd/issues)
in this fork, and say which backend. On AMD, give the GPU, where the runtime
package came from, and the excerpt from `DLSSVideoPlayer.log` — the log is a file
beside the exe, not console output. An upstream NVIDIA-backend bug belongs on
upstream's [tracker](https://github.com/2600th/dlss5-video-player/issues), which
is the one that reaches the player's author. Remove private paths and signed
media URLs from logs first.

## Credits and license

A fork of [DLSS 5 Video Player](https://github.com/2600th/dlss5-video-player) by
2600th, which in turn started from
[DLSS 5 Video Player by Jessica Natalia Mods](https://gitlab.com/JessicaNataliaMods/dlss-5-video-player/).
The player, its compare views and its documentation are upstream's work; what this
fork adds is the AMD backend.

Project source is [MIT](LICENSE). Third-party binaries, the game and film
footage in `docs/media/` and the trademarks in it keep their own terms; see
[THIRD_PARTY.md](THIRD_PARTY.md). The AMD runtime package and its model weights
are their own drop, described in [third_party/lmxxf](third_party/lmxxf/README.md).
