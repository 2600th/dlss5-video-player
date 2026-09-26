# Improvement task list

_Audited against 0.26.0 (13f92b7) on 2026-09-25. Status re-checked against
0.26.2 (335edb3) on 2026-09-26: every item below is still open unless it says
what part is done. File:line citations are as of the audit and have drifted by
up to ~75 lines in `main.cpp`; search for the named symbol._

Open work only, highest priority first. Completed items are removed rather
than ticked; what shipped is in `CHANGELOG.md` and git history. When a task
lands, delete it from this file in the same change.

**IDs are never reused.** Source comments cite them (`P2.8`, `P3.7`), so a
new task takes the next free number in its tier even when lower numbers are
free. Next free: **P0.19, P1.35, P2.43, P3.14**.

Sources: three code audits (neural helper/cache/runtime; decode, playback,
audio and export; app shell, build, CI and release) and two web surveys of the
DLSS 5 landscape, on 2026-09-25, plus the open items carried over from the
2026-09-23 list.

## How to read it

| Mark | Meaning |
| --- | --- |
| **P0** | A bug users hit, a security gap, or a published promise that is broken. Fix before the next release. |
| **P1** | Reliability, CI, release hygiene and docs. The next few weeks. |
| **P2** | High-value features, from the research. Each is justified on its own. |
| **P3** | Large or strategic, or waiting on something outside this repo. |
| 🧪 | An audit agent reproduced it (pinned FFmpeg 9.0.1, `dumpbin`, or `gh`) |
| 🔍 | Audit finding cited to file:line, not independently re-read |

Each task has the same four parts: **Problem**, **Where**, **Fix**, **Test**.
**Impact** names what it changes: **Player** (what the viewer sees and
feels), **Pipeline** (render, cache, export, helper), **Release** (packaging,
CI, supply chain), **Site** or **Docs**. File references are relative to
`src/` unless a path is given.

**Baseline.** Measured at 0.26.2 on an RTX 5090 with the RTX Video SDK staged:
Release build clean at **0 warnings /W4**; CTest **33 registrations, all pass**
(14 portable, 19 hardware); `site/test.ps1` 61 pass.

## The rule that shapes this list

> **Quality is the default. It is never traded for speed without the user
> asking.**

- **Quality-neutral** work (copies, spins, polling, allocation churn) ships
  unconditionally, with byte-identical output.
- **Quality-affecting** work (processing scale, encoder settings, temporal
  blend) ships as a named ladder whose default is the near-best rung, with
  the measured cost printed beside each rung. A default never moves down the
  ladder to buy speed.
- Anything that changes output pixels becomes a cache-key term.

---

## Summary

| ID | Task | Effort | Impact | |
| --- | --- | :---: | --- | :---: |
| **P0** | **Fix before the next release** | | | |
| | _Nothing open_ | | | |
| **P1** | **Next** | | | |
| [P1.24](#p124) | A retiring resident helper accepts a job, which then fails | M | Pipeline | 🔍 |
| [P1.25](#p125) | Audio that starts after the video plays early | S | Player | 🧪 |
| [P1.26](#p126) | Anamorphic sources export stretched | S | Pipeline | 🔍 |
| [P1.27](#p127) | YouTube open, seek or reload drops 1-4 frames | S | Player | 🔍 |
| [P1.28](#p128) | Inherited handles can hang an export and the UI | M | Pipeline | 🔍 |
| [P1.29](#p129) | Load-time DLLs are not pinned to System32 | S | Release | 🧪 |
| [P1.30](#p130) | The complete zip cannot be traced to a commit | S-M | Release | 🧪 |
| [P1.31](#p131) | GPU and audio tests never run in CI | M | Release | 🧪 |
| [P1.32](#p132) | Refresh `RELATED_PROJECTS.md` | S | Docs | |
| [P1.33](#p133) | Small hardening and doc drift (checklist) | S | All | 🔍 |
| [P1.34](#p134) | FFmpeg source availability and bundled-library notices | S-M | Release | |
| **P2** | **High-value features** | | | |
| | _Comparison and review_ | | | |
| [P2.27](#p227) | Save a comparison clip | S-M | Player, Pipeline | |
| [P2.28](#p228) | Compare two neural presets on one frame | M | Player | |
| [P2.29](#p229) | Flicker-map compare view | M | Player | |
| [P2.30](#p230) | Per-frame metric lane and worst-frames navigator | M | Player, Pipeline | |
| [P2.31](#p231) | Loop In/Out and slow-motion playback | S-M | Player | |
| [P2.32](#p232) | Scopes and a pixel probe | M | Player | |
| [P2.33](#p233) | Comparison frame pack | S-M | Player | |
| [P2.34](#p234) | Several pinnable magnifiers | S-M | Player | |
| | _Render and export_ | | | |
| [P2.35](#p235) | Resume and join partial renders across sessions | M-L | Pipeline | |
| [P2.36](#p236) | Opt-in face protection mask | M-L | Player, Pipeline | |
| [P2.37](#p237) | Opt-in: bake the Mix and mask into exports | M | Pipeline | |
| [P2.38](#p238) | Measure DLSS SR presets L and M | S | Pipeline | |
| [P2.39](#p239) | RTX VSR on the neural render | M | Player, Pipeline | |
| [P2.40](#p240) | Opt-in subtitle burn-in for MP4 and GIF | M | Pipeline | |
| | _Input and convenience_ | | | |
| [P2.41](#p241) | Clipboard copy and paste | S | Player | |
| [P2.42](#p242) | YouTube captions | S | Player | |
| | _Carried over_ | | | |
| [P2.4](#p24) | Evaluate Video Depth Anything through the guide harness | M-L | Pipeline | |
| **P3** | **Strategic or large** | | | |
| [P3.9](#p39) | HDR output of the render | L | Pipeline | |
| [P3.10](#p310) | Processing scale above 100% as an export-only rung | M | Pipeline | |
| [P3.11](#p311) | Compare against an external file | M-L | Player | |
| [P3.12](#p312) | Network sources beyond YouTube | M | Player | |
| [P3.13](#p313) | Render queue (owner's decision) | M | Pipeline | |
| [P3.5](#p35) | Prefer NVIDIA's signed runtime on RTX 50 | M | Pipeline, Release | |
| [P3.6](#p36) | Neural optical flow as an export-only rung | M-L | Pipeline | |
| [P3.8](#p38) | Repository media hygiene | S | Release | |

---
---

# P0 — Fix before the next release

Nothing open. P0.11-P0.18 landed on 2026-09-25 (branch `fix/p0-all`).

---

# P1 — Next

<a id="p124"></a>
### P1.24 · A retiring resident helper accepts a job, which then fails

`M` · **Pipeline** · 🔍

- **Problem.** The helper decides to exit (30 s idle, or `JobInvalidated`,
  including a session log over 2 MiB) and tears down while still alive with
  its command pipe open. The parent's `Resident()` only checks the process,
  so it reuses it and `Send` succeeds; the job is never read. The helper exits
  0 with no result, `RunAttempt` reports "incomplete metadata", and
  `RunWithRelaunches` does not relaunch for `Protocol`. The user sees a warm
  toggle about 30 s after the last job, or the next hole-fill job, fail.
- **Where.** `ResidentWorkerLoop.h:359, 370-374`; `NeuralWorkerMain.cpp:451-455,
  676-680`; `NeuralWorker.cpp:1692, 1777, 1783-1805, 1847, 1880-1884,
  1027-1031`.
- **Fix.** Either give idle and retiring exits their own exit code and
  re-dispatch "reused, exited with it, nothing received" through the two-try
  launch loop, or have the helper announce it is retiring so the parent drops
  the session first.
- **Test.** Dispatch to a helper that is alive but has decided to exit
  (`tests/NeuralWorkerTests.cpp:2413-2663` cover only the loop).

<a id="p125"></a>
### P1.25 · Audio that starts after the video plays early

`S` · **Player** · 🧪 · _0.5 s early, reproduced_

- **Problem.** With no seek there is no `-ss`, and raw `f32le` output drops
  the audio stream's start offset while the clock treats byte 0 as the seek
  base. Audio runs ahead by the offset until the first seek.
- **Where.** `AudioPlayer.cpp:344, 361-367`. The repo's own test notes it:
  `tests/CachedExportTests.cpp:733-737`.
- **Fix.** `-af aresample=async=1:first_pts=0` with `-copyts`, or shift the
  seek base by each stream's `start_time`.
- **Test.** An MKV with video at 0.0 s and audio at 0.5 s.

<a id="p126"></a>
### P1.26 · Anamorphic sources export stretched

`S` · **Pipeline** · 🔍

- **Problem.** The sample aspect ratio is probed but used only for the
  on-screen display ratio. The rawvideo encode sets no SAR and direct NVENC
  hard-codes square pixels, so a DVD rip (720x480 at 32:27) or HDV (1440x1080
  at 4:3) looks right in the player and exports stretched.
- **Where.** `VideoDecoder.cpp:458-459`, `main.cpp:6806`,
  `MediaPipeline.cpp:578-582`, `NvencDirectPolicy.h:278-279`.
- **Fix.** Carry the SAR into the encode (`setsar`/`-aspect`, and
  `darWidth`/`darHeight` for NVENC), or apply it at export with `-aspect`.
- **Test.** An anamorphic export keeps its display aspect.

<a id="p127"></a>
### P1.27 · YouTube open, seek or reload drops 1-4 frames

`S` · **Player** · 🔍

- **Problem.** The candidate decoder keeps reading ahead while its renderer is
  built; `Swap` then stops both queues with `QueueBuffer::Discard` and clears
  them, after `m_ffmpegEmittedFrames` counted those frames. Playback jumps by
  up to ~133 ms at 30 fps with no discontinuity flag, and temporal history
  crosses the gap.
- **Where.** `VideoDecoder.cpp:144-145, 159, 1481-1482`;
  `main.cpp:10797, 10952`.
- **Fix.** In `Swap`, stop with `QueueBuffer::Keep`, swap
  `m_frameQueue`/`m_frameTerminal`, and restart with `Keep`.
- **Test.** The frame after a swap is the next frame.

<a id="p128"></a>
### P1.28 · Inherited handles can hang an export and the UI

`M` · **Pipeline** · 🔍 · _plausible race_

- **Problem.** Several children are created with `bInheritHandles=TRUE` and no
  handle list, so each inherits every inheritable handle alive at that moment.
  A seek during an export spawns the playback ffmpeg, which then holds the
  export probe's stdout pipe open. The drain loop blocks until that child dies
  and `CancelExport` joins the worker on the UI thread, which can hang the
  app. The milder form is a 10 s audio stall.
- **Where.** Spawns: `VideoDecoder.cpp:329-330, 963-964`,
  `AudioPlayer.cpp:101, 373`, `SubtitleOverlay.cpp:303`. Drain:
  `MediaPipeline.cpp:301-304, 378-390`. Join: `main.cpp:3487`. Stall:
  `AudioPlayer.cpp:115-132`.
- **Fix.** `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` everywhere, as `MediaPipeline`
  and `YouTubeResolver` already do; make the drain bounded and non-blocking.
- **Test.** A policy test that every spawn passes a handle list.

<a id="p129"></a>
### P1.29 · Load-time DLLs are not pinned to System32

`S` · **Release** · 🧪 · _`dumpbin`: Dependent Load Flag 0000_

- **Problem.** The player statically imports `dxgi`, `d3d12`, `d3d11`,
  `D3DCOMPILER_47`, `dbghelp`, `VERSION`, `WINHTTP` and `dwmapi`, none of them
  KnownDLLs. `SetDefaultDllDirectories` runs after they are resolved, so a
  `dxgi.dll` beside the exe (the old layout's unsigned ReShade proxy, or a
  planted one) loads before the guard that rejects it.
- **Where.** `CMakeLists.txt:242`; `main.cpp:12431` (too late),
  `main.cpp:1308-1309` (guard).
- **Fix.** `/DEPENDENTLOADFLAG:0x800` on `DLSSVideoPlayer` only, or delay-load
  those DLLs. Not on `NeuralWorker`, which needs the proxy `dxgi.dll` beside
  it.
- **Test.** A CTest check that reads the load-config flag from the built exe.

<a id="p130"></a>
### P1.30 · The complete zip cannot be traced to a commit

`S-M` · **Release** · 🧪 · _`gh run`, `gh api rulesets`, `git ls-remote`_

- **Problem.**
  - `Assert-ReleaseBuild` checks only the version resource: no clean-tree
    check, no "HEAD is the tag" check, and no commit in
    `PACKAGE_MANIFEST.txt`.
  - The `dlss5-video-player-v0.26.0` tag moved from a4a3411 to 13f92b7 after
    release run 36047815691 failed, and `dlss5-video-player-v0.26.2` from
    581a13a to 5dbb3b9 after run 36229669021 failed on a stale stamp; there is
    no tag ruleset to stop that.
  - README:53-56 says only the core zip has an attestation, yet
    `attest-release-asset.yml` exists for the complete one.
- **Where.** `tools/package_release.ps1:83-125`,
  `.github/workflows/attest-release-asset.yml`.
- **Fix.** Refuse a dirty tree or a HEAD that is not the tag; write the commit
  into the manifest; add a tag ruleset; run the attest workflow for the
  complete zip, or correct the README.
- **Test.** Packaging from a dirty tree fails.

<a id="p131"></a>
### P1.31 · GPU and audio tests never run in CI

`M` · **Release** · 🧪

- **Problem.** `gpu-tests.yml` needs a `[self-hosted, windows, gpu]` runner
  that does not exist; runs sit queued for 24 h and are cancelled.
  `build.yml` and `release.yml` run the `portable` preset, which excludes
  `gpu|audio`. Neural renders, frame generation, direct NVENC, the export
  matrix and the audio clock never get a CI assertion, and a tag can ship
  with neural rendering broken.
- **Where.** `.github/workflows/gpu-tests.yml:36`, `build.yml:112`,
  `release.yml:86`, `build_windows.bat:128`.
- **Fix.** Register the runner, or make packaging require a recent passing
  JUnit report from the full suite.

<a id="p132"></a>
### P1.32 · Refresh `RELATED_PROJECTS.md`

`S` · **Docs**

- **New since the last survey:**
  [DLSS5Tool](https://github.com/banbanzhige/DLSS5Tool) (queue, RTX Video
  2x/4x, RAFT flow),
  [DLSS5-Image-Converter](https://github.com/criso2hd-alt/DLSS5-Image-Converter)
  ("Compare styles", tiled Ultra Detail),
  [MPCVR-DLSS5](https://github.com/HumbleUser33/MPCVR-DLSS5),
  [PotPlayer plugin](https://github.com/222222222l/DLSSNR-Potplayer-Plugin),
  [OpenPlayer plugin](https://github.com/AreChen/openplayer-dlssnr),
  [DaVinci Resolve OFX](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5)
  (Output Mix, Difference x10),
  [Nuke plug-in](https://github.com/2148-wq/DLSS5-for-Nuke),
  [Neural-coprocessor](https://github.com/maohgad-web/Neural-coprocessor)
  (second GPU).
- **Changed:** Visual Enhancer
  [v11.0](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v11.0)
  (ProRes/FFV1, Face/Skin Protection, scale up to 200%); NeuralScreen
  [v2.1.4](https://github.com/perseval-BLR/NeuralScreen/releases/tag/v2.1.4)
  (display matrix, EXIF, ICC).
- **Gone quiet:** Zonnery's player, the Blueforcer ComfyUI node.
- **Still true:** no public DLSS 5 SDK (DLSS SDK 310.9.1 and Streamline 2.14.1
  add nothing for it).

<a id="p133"></a>
### P1.33 · Small hardening and doc drift

`S` each · **All** · 🔍

- [ ] The detached cache-eviction thread can outlive static destruction at
      exit (`main.cpp:2876-2912` → `NeuralPreflight.cpp:604-607`). Two audits
      flagged it.
- [ ] The runtime-lock hash is memoised on path, size and write time
      (`RuntimeLock.cpp:163`, `NeuralCache.cpp:877-909`): a same-size,
      same-timestamp swap passes for the rest of the process.
- [ ] A source path over 2048 characters is refused by the resident helper
      with no single-shot fallback (`NeuralWorker.cpp:1651-1654`).
- [ ] `--fps` is sent at 6 decimals (`NeuralWorker.cpp:1180`).
- [ ] `--output WxH` has no upper clamp (`PlayerCommandLine.h:48-49`).
- [ ] "Save converted video" does not re-count the output's frames as
      `MuxStageExport` does.
- [ ] `.dlss-export-*.tmp` files are left beside the output after a crash.
- [ ] Local HLS and concat playlists open without a `-protocol_whitelist`.
- [ ] `PickExportFile` offers all five formats whatever the source is.
- [ ] `tools/fetch_ui_assets.ps1:62` calls `tar.exe` without a full path.
- [ ] The release workflow does not run the ASan/analyze `quality` job.
- [ ] `SECURITY.md:23` still says "RTX 40 compatibility modification"; the
      locked runtime is the universal SF-v2 build.
- [ ] "Save converted video" returns a note when it leaves subtitles out of
      an MP4, but the success dialog never shows it (the stage export only
      logs its note).
- [ ] Whole-file MP4 export still re-encodes every audio track to AAC 192k;
      the stage export copies audio MP4 can hold (`AppendSourceStreams`,
      `encodeAudio`). One argument to align them.
- [ ] A Super Resolution-only stage export is not pre-checked against the
      runtime lock (the helper still refuses stray modules at startup).
- [ ] The Media Foundation packed-row fallback walks a negative-stride
      (bottom-up) frame top-down (`MediaFoundationSamplePolicy.h`); no fixture
      proves which is right.
- [ ] Nothing tests that the export dialog wires `releaseResidentHelper`;
      the ordering is tested through `RunStageExport` only.

---

<a id="p134"></a>
### P1.34 · FFmpeg source availability and bundled-library notices

`S-M` · **Release** · _left over from P0.17_

**Blocker:** a licensing decision only the owner can make.

- **Problem.** The complete zip now ships the GPLv3 text and
  `THIRD_PARTY_LICENSES/ffmpeg.txt` points at the FFmpeg commit and each
  statically linked library's upstream. GPLv3 §6(d) still leaves the
  distributor responsible for the source staying available, and the build
  scripts are part of it; gyan.dev's archive carries only its README, which
  gives no x264 commit and no versions for zlib, bzip2, lzma or GnuTLS's
  dependencies. Permissive libraries inside `ffmpeg.exe` (aom, vpx, opus,
  webp, ...) also need their notices shipped.
- **Options.** Host the exact source archives and build scripts beside the
  complete zip; make a §6(b) written offer; or drop FFmpeg from the complete
  zip.
- **Fix.** Whichever is chosen, collect the permissive notices into
  `THIRD_PARTY_LICENSES/` and add them to both packager lists and
  allowlists.

---

# P2 — High-value features

Each of these is quality-first. Anything that changes pixels is opt-in,
becomes a cache-key or receipt term, and any trade-off ships as a ladder with
a labelled default. What users complain about most with DLSS 5 is **faces
changing, flicker, and colour shift**
([Digital Foundry via ResetEra](https://www.resetera.com/threads/digital-foundry-dlss-5-tested-nba-2k27-image-quality-benchmarks-mods-and-more.1625434/),
[Held Games](https://heldgames.com/guides/dlss-5-official-vs-mod),
[Visual Enhancer #53](https://github.com/Merserk/dlss5-visual-enhancer/issues/53));
the comparison features below are aimed at making those visible.

## Comparison and review

<a id="p227"></a>
### P2.27 · Save a comparison clip

`S-M` · **Player, Pipeline**

- **What.** Split, wipe, side by side or 2x2 as a labelled MP4 over In/Out,
  built from the cached render plus the source (`hstack`/`overlay`/`drawtext`).
- **Why.** "Off vs on" clips are how DLSS 5 is judged
  ([TechRadar](https://www.techradar.com/computing/nvidias-dlss-5-is-going-viral-for-all-the-wrong-reasons-here-are-the-5-most-controversial-examples-of-the-ai-powered-breakthrough-in-action)).
  The player saves comparison stills only; Visual Enhancer's 2-Up is preview
  only.
- **Where.** `ExportPipeline.h`, `MediaPipeline.cpp`, `AppMenu.cpp`,
  `main.cpp`, `RenderCommandLine.h`.

<a id="p228"></a>
### P2.28 · Compare two neural presets on one frame

`M` · **Player**

- **What.** Preset A vs preset B in two panes with shared zoom, on the paused
  frame first; pane B comes from a second settings key in the cache.
- **Why.** Choosing a preset today means re-rendering and remembering.
  DLSS5-Image-Converter ships "Compare styles".
- **Where.** `NeuralPresets.h`, `CompareViewPolicy.h`, `AppMenu.cpp`.

<a id="p229"></a>
### P2.29 · Flicker-map compare view

`M` · **Player**

- **What.** Per pixel, how much more the render changes than the source after
  following motion, amplified. Reuses the warp and source-residual test in
  `TemporalStabilityShader.h`.
- **Why.** Shimmer and faces not holding are the top complaints
  ([VE #31](https://github.com/Merserk/dlss5-visual-enhancer/issues/31),
  [VE #53](https://github.com/Merserk/dlss5-visual-enhancer/issues/53)).
  Only an aggregate number exists (`main.cpp:10365`); no competitor has a map.
- **Where.** `D3D12Renderer.cpp`, `CompareViewPolicy.h`, `CompareBarPolicy.h`,
  `AppMenu.cpp`.
- **Risk.** Flow errors at occlusions read as flicker; gate them as the
  stability pass does.

<a id="p230"></a>
### P2.30 · Per-frame metric lane and worst-frames navigator

`M` · **Player, Pipeline**

- **What.** A thin lane on the timeline plotting a per-frame metric, and
  "jump to the worst N frames".
- **Why.** The standard review pattern
  ([FFMetrics](https://github.com/fifonik/FFMetrics/issues/26)). The receipt
  stores aggregates only; the only per-frame series are timings
  (`OfflineNeuralRenderer.cpp:157`).
- **Where.** `TemporalMetrics.h`, `NeuralReceipt.cpp` (downsampled sidecar),
  `TimelinePolicy.h`, `main.cpp`.

<a id="p231"></a>
### P2.31 · Loop In/Out and slow-motion playback

`S-M` · **Player**

- **What.** Loop the marked range; play at 1/2, 1/4 and 1/8 speed on the
  exact cached frames (no interpolation), audio muted.
- **Why.** Temporal artefacts are judged by looping a short range slowly.
  ICAT and [video-compare](https://github.com/pixop/video-compare) both do it;
  nothing in `src/` does.
- **Where.** `RangeSelection.cpp`, `PlaybackCadence.h`,
  `PlaybackTickPolicy.h`, `SynchronizedPlayback.cpp`, `AppMenu.cpp`.

<a id="p232"></a>
### P2.32 · Scopes and a pixel probe

`M` · **Player**

- **What.** Waveform, vectorscope and histogram for original vs render, and a
  pixel probe showing both values under the cursor.
- **Why.** Makes the colour-shift complaints measurable ("washed out",
  saturation loss, pink lips). video-compare has them; `src/` has none.
- **Where.** `D3D12Renderer.cpp`, `UiLayout.cpp`, `AppMenu.cpp`.

<a id="p233"></a>
### P2.33 · Comparison frame pack

`S-M` · **Player**

- **What.** N frames (at markers, random, or the worst flicker) saved as
  source and render PNG pairs in the slow.pics layout, with a local HTML
  viewer. Local only; upload only on explicit request.
- **Why.** The encoding community's standard workflow
  ([thewiki.moe](https://thewiki.moe/tutorials/comparison/),
  [Pear](https://github.com/rlaphoenix/Pear)).
- **Where.** `CompareImageIO.h`, `main.cpp`, `AppMenu.cpp`.

<a id="p234"></a>
### P2.34 · Several pinnable magnifiers

`S-M` · **Player** · _lower value; the single loupe covers most of it_

- **What.** Drop two or three pinned magnifiers (eyes, a texture patch) and
  read them all on one frame.
- **Why.** [Improve-ImgSLI v9.0.0](https://github.com/Loganavter/Improve-ImgSLI/releases/tag/v9.0.0).
- **Where.** `CompareViewPolicy.h`, `D3D12Renderer.cpp`, `UiLayout.cpp`.

## Render and export

<a id="p235"></a>
### P2.35 · Resume and join partial renders across sessions

`M-L` · **Pipeline** · _quality-neutral_

- **What.** Treat an earlier or interrupted run's segments as coverage when a
  video is reopened, and join partial renders, instead of rendering again.
- **Why.** Both are stated limits in the README. Losing long renders is the
  category's loudest complaint
  ([Topaz: 17 hours lost](https://community.topazlabs.com/t/pause-resume-threw-away-17-hours-of-rendered-footage/83266)).
  `NeuralSegmentIndex` already tags segments by run and merges runs.
- **Where.** `NeuralCache.cpp`, `NeuralSegmentIndex.h`,
  `OfflineNeuralRenderer.cpp`.

<a id="p236"></a>
### P2.36 · Opt-in face protection mask

`M-L` · **Player, Pipeline** · _off by default_

- **What.** Faces from Windows' built-in
  [`FaceDetector`](https://learn.microsoft.com/en-us/uwp/api/windows.media.faceanalysis.facedetector)
  (no model download) feed the existing Mix mask frame by frame; the render
  report gains a face-region colour shift.
- **Why.** Missing masks are the cited cause of identity changes
  ([Held Games](https://heldgames.com/guides/dlss-5-official-vs-mod)); Visual
  Enhancer ships Face/Skin Protection; the player's own tooltip says Skin
  structure "is not face protection" (`Localization.h:363`).
- **Where.** A new face-mask policy and detector, `CompareMaskPolicy.h`,
  `D3D12Renderer.cpp`, `TemporalMetrics.h`, `NeuralReceipt.cpp`.
- **Risk.** Misses profile and small faces, so the mask pops; needs a hold and
  feather over time.

<a id="p237"></a>
### P2.37 · Opt-in: bake the Mix and mask into exports

`M` · **Pipeline**

- **What.** Apply the Mix and mask to "Save converted video" (e.g.
  `maskedmerge`), recorded in the receipt.
- **Why.** Today they affect only what you watch (`docs/USAGE.md:256-257`).
  The Resolve filter exposes Output Mix; it is the direct answer to "too
  much" on faces.
- **Where.** `ExportPipeline.h`, `MediaPipeline.cpp`, `CompareMaskPolicy.h`.

<a id="p238"></a>
### P2.38 · Measure DLSS SR presets L and M

`S` · **Pipeline** · _measurement first_

- **What.** Run presets L and M through the six-clip harness against bicubic.
  Add them as rungs only if they win; otherwise re-date the README's verdict.
- **Why.** `DLSSBackend.cpp:225-232` forces preset K in every mode. The
  2nd-generation transformer
  ([NVIDIA, 2026-01-06](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/))
  post-dates the "loses to bicubic" measurement, and the pinned 310.9.1 DLL
  defines L and M.
- **Where.** `DLSSBackend.cpp`, `UpscalingPolicy.h`, `docs/measurements/`.

<a id="p239"></a>
### P2.39 · RTX VSR on the neural render

`M` · **Player, Pipeline** · _must beat bicubic on the harness first_

- **What.** RTX VSR as a display upscaler and export stage after the neural
  pass, not only on the original.
- **Why.** `VsrPolicy.h` runs VSR on the original only. DLSS5Tool, Visual
  Enhancer's Upscale tab and a fork do VSR after the neural pass;
  [NeuralScreen #125](https://github.com/perseval-BLR/NeuralScreen/issues/125)
  asks for it.
- **Where.** `VsrEngine.cpp`, `UpscalingPolicy.h`, `ExportPipeline.h`,
  `OfflineNeuralRenderer.cpp`. Becomes an export key term.

<a id="p240"></a>
### P2.40 · Opt-in subtitle burn-in for MP4 and GIF

`M` · **Pipeline** · _lower value; MKV already carries subtitles_

- **What.** Reuse the playback subtitle compositing in the export path, off by
  default.
- **Why.** Listed as unsupported in the README; soft subtitles do not survive
  MP4-only platforms or re-uploads. Nothing is burned in today
  (`MediaPipeline.cpp:1014`).
- **Where.** `ExportPipeline.h`, `SubtitleOverlay.cpp`, `AppMenu.cpp`.

## Input and convenience

<a id="p241"></a>
### P2.41 · Clipboard copy and paste

`S` · **Player**

- **What.** `Ctrl+Shift+C` copies the comparison image with its footer;
  `Ctrl+V` opens a pasted image, file or YouTube link.
- **Why.** Visual Enhancer and DLSS5-Image-Converter both paste; here the
  clipboard is read only inside the YouTube dialog (`main.cpp:468`).
- **Where.** `main.cpp`, `CompareImageIO.h`, `DroppedFilesPolicy.h`.

<a id="p242"></a>
### P2.42 · YouTube captions

`S` · **Player**

- **What.** Ask yt-dlp for the caption track URLs and draw them with the
  existing WebVTT overlay.
- **Why.** A pasted link is one of the three ways to open media and gets no
  subtitles; the resolver never requests captions
  (`YouTubeResolver.cpp:888`).
- **Where.** `YouTubeResolver.cpp`, `SubtitleOverlay.cpp`, `SubtitlePolicy.h`.

## Carried over

<a id="p24"></a>
### P2.4 · Evaluate Video Depth Anything through the guide harness

`M-L` · **Pipeline** · _the harness half shipped: `depth=file:` / `mv=file:` guide modes_

**Blocker:** needs torch and a model download, so it is the owner's to run.

The harness is in: offline depth and flow can be fed to the worker from
files and A/B'd through `tools/benchmark` (see its README for the exact
steps), with near/far clips `depth-pan` and `depth-subject`. Feeding the
built-in guides back through files is byte-identical.

What is left: generate Video Depth Anything Small depth for the corpus,
normalised over each clip (not per frame), and A/B it. The first harness
result sets expectations: even *exact* synthetic depth moves the output by at
most 0.03 dB PSNR, so depth has little headroom on this pipeline; flow is the
more promising guide (P3.6).

---

# P3 — Strategic or large

<a id="p39"></a>
### P3.9 · HDR output of the render

`L` · **Pipeline** · _opt-in; measure before shipping_

- **What.** Re-expand the SDR render with the source's per-pixel HDR/SDR ratio
  for HDR10 export and HDR displays, or use RTX Video HDR (TrueHDR; headers in
  `external/rtx-video-sdk/include/nvsdk_ngx_defs_truehdr.h`, unused in
  `src/`).
- **Why.** Requested in NeuralScreen #79, Visual Enhancer #35 and
  video2dlssnr #4; Topaz shipped SDR to HDR in May 2026
  ([Topaz](https://www.topazlabs.com/news/the-expansion-release)).
- **Where.** `HdrPolicy.h`, `HdrToneMapGpu.cpp`, `NvencDirect.cpp`,
  `ExportPipeline.h`.
- **Risk.** Halos where the model relit the scene.

<a id="p310"></a>
### P3.10 · Processing scale above 100% as an export-only rung

`M` · **Pipeline** · _run the guide harness first_

- **What.** A supersampled neural pass (125-200%) for export, with its
  measured cost printed beside the rung. The live ladder stays `{100, 75, 50}`
  (`UpscalingPolicy.h:188`).
- **Why.** Visual Enhancer offers 125-200% (its #46 asked for it);
  DLSS5-Image-Converter's Ultra Detail tiles at a larger size.
- **Risk.** The runtime tops out at 7680x4320, landscape only.

<a id="p311"></a>
### P3.11 · Compare against an external file

`M-L` · **Player**

- **What.** Another tool's output or an older render as the compare side, with
  a ±frame offset.
- **Why.** video-compare supports several right-hand videos with a time
  shift; Improve-ImgSLI v10 has a Multi Compare grid; users already compare
  renderer versions by hand.
- **Where.** `VideoDecoder.cpp`, `SynchronizedPlayback.cpp`,
  `D3D12Renderer.cpp`, `CompareViewPolicy.h`.

<a id="p312"></a>
### P3.12 · Network sources beyond YouTube

`M` · **Player** · _security-sensitive_

- **What.** Twitch VODs and clips, Vimeo, direct https.
- **Why.** Visual Enhancer's Live mode accepts them; the resolver allows
  YouTube hosts only (`YouTubeResolver.cpp:598-620`).
- **Risk.** Keep a per-host allowlist and the existing yt-dlp hardening.

<a id="p313"></a>
### P3.13 · Render queue

`M` · **Pipeline**

**Blocker:** the owner's decision. DLSS5Tool, Visual Enhancer and REAL all
have one; the README lists it as unsupported, and watch folders are parked on
the grounds that `--render` covers batch work.

---

<a id="p35"></a>
### P3.5 · Prefer NVIDIA's signed runtime on RTX 50

`M` · **Pipeline, Release** · unverified

**Blocker:** nobody has confirmed the signed DLL is usable this way; that
needs an RTX 50 card.

Driver 616.64 enables DLSS 5 officially on RTX 50 and downloads its models to
`%ProgramData%\NVIDIA\NGX\models`. If a usable signed neural-rendering DLL can
be found there for an app that is not on NVIDIA's list, preferring it on RTX
50 would remove the "modified, unsigned" warning for those users. Record
which runtime ran in the receipt. Keep the community build for RTX 20-40.

---

<a id="p36"></a>
### P3.6 · Neural optical flow as an export-only rung

`M-L` · **Pipeline** · _gated by P2.4's harness_

**Blocker:** no evidence yet that better flow helps; motion vectors measured
only +0.297 dB on the cuts-motion clip.

SEA-RAFT's smallest model runs 1080p at about 21 fps on a 3090, and an ONNX
export exists. That is too slow for live playback, but acceptable for export.
Keep NVOFA for live. Before any C++ work, run the cheap offline experiment:
precompute SEA-RAFT flow in Python and feed it through the P2.4 guide harness
(`mv=file:`). Build the rung only if that shows a gain. Outside evidence:
[DLSS5Tool](https://github.com/banbanzhige/DLSS5Tool) publishes RAFT's
temporal residual at 1.560 against NVOFA's 1.610.

---

<a id="p38"></a>
### P3.8 · Repository media hygiene

`S` · **Release**

**Blocker:** moving history to LFS or release assets rewrites it, which is the
owner's call.

- **Media in history:** `docs/media/neural-comparison-demo.mp4` is stored
  five times (9.1, 7.9, 7.2, 6.3 and 5.3 MB; the 5.3 MB one lives on as the
  benchmark fixture). There is also a 4.2 MB webp; the pack is 45.7 MiB.

Serve media that can be re-shot from release assets or LFS, and run `git gc`.

---
---

# Parked

Researched and deliberately **not** being done. They are recorded here so they
are not proposed again.

| Item | Why not |
| --- | --- |
| **Custom model loading (.pth/.onnx)** | Structurally impossible: NGX feature 18 has fixed weights. chaiNNer-style tools are a different category. |
| **Watch folders** | Requested at Topaz since 2022 and never shipped, with no sign that users urgently need it. The `--render` command line covers the batch case. |
| **Stabilization, deinterlacing, colorization, face restoration** | Each needs a model we cannot obtain. Inferior versions dilute a single-model product. |
| **8K/16K output, 480 fps frame generation** | Marketing checkboxes that no panel can show. Cadence-aware multiple selection is the right design, so say that instead. |
| **Cloud rendering / credits / tiering** | Being MIT and free is an asset against a closed competitor. |
| **Linux or mobile ports, multi-GPU splitting of one render, "auto" model recommendation** | Feature 18 on Windows D3D12 is the product. Separate GPU choices for AI and for NVENC is the useful form (`GpuPreference.cpp`). |
| **Running the model on AMD or Intel** | Community projects do it (DLSS-NR-on-AMD, an Xe2 port); the product is RTX-only by design. |
| **Full zero-copy decode/encode rewrite** | The ffmpeg child is load-bearing: codec coverage, process isolation for the runtime lock, and the benchmark harness. Only the narrow encode side was worth doing, and it shipped as direct NVENC. |
| **AV1 export** | Needs a 40-series NVENC. The High rung's HEVC Main10 covers the need everywhere. |
| **GPU-utilization cap for RTX VSR** | The SDK exposes only `VSR.QualityLevel` 0-4; NVIDIA App's control is a driver setting for its Auto mode, and the player already has a Low-Ultra ladder. |
| **"Guide preview" view** | Already shipped as the Motion vectors and Depth views. |

---
---

# What is already strong

Verified in the 2026-09-20, 2026-09-23 and 2026-09-25 audits. **Do not "fix"
these, and do not suggest them again.**

**Architecture and protocol**
- **The IPC wire format:** magic number plus an exact version gate,
  `static_assert` on every struct size, and decoders that reject trailing
  bytes, oversized counts and overflow. One shared header rather than two
  schemas.
- **No reachable command injection:** no `cmd.exe`, `system()` or `_popen`.
  `lpApplicationName` is always a handle-verified absolute path. yt-dlp runs
  with `--no-config --no-plugin-dirs`, and URLs are allowlisted and filtered.
  Playback's FFmpeg fallback walks only absolute `PATH` entries, never the
  current directory.
- **Helper TOCTOU is closed:** `FILE_FLAG_OPEN_REPARSE_POINT`, canonicalised
  through the same handle, which stays open with `FILE_SHARE_READ` across the
  whole resolve.
- **The cache never serves a mismatched render.** The key covers source,
  runtime, settings, driver, model store, range, guides and pipeline terms;
  `CachedRenderEvidence` and the final settings recheck fail closed, and so
  do disk-full and short writes.
- **No cache path is ever read from disk.** Every path is
  `root/<bucket>/<hex-digest>`, re-checked with a component-wise descendant
  test.
- **Publication is a directory rename**, and SHA-256 covers the payload and
  both sidecars on every read.
- `SegmentWriter`'s three-thread handshake, `CompletionRegistry`'s scalar
  tokens, and `ParallelFor.h`'s pool have each been walked for every
  interleaving and hold. `AtomicFile.h`, `KillOnCloseJob.h`,
  `SendShutdownAndCloseBounded` and `MetadataPipe` teardown are clean.

**GPU and media**
- **D3D12 slot discipline:** a slot wait before every mapped write and
  allocator reset, and a drain before releasing DLSS.
- **DRED is wired up**, and device-removed recovery (`RecoverUnusableRenderer`)
  holds.
- **GDI objects are always released**, including on failure paths.
- NVDEC runs on both the playback and offline paths.
- `NvencDirect`, `HdrToneMap`/`HdrToneMapGpu`, `Nv12Convert`,
  `CompareImageIO` and `WasapiRenderer` locking are clean.

**Already optimised; these do not need doing again:**
- the capture readback ring;
- NV12 over the pipe;
- guide fan-out via per-worker semaphores;
- segment rotation with a warm encoder;
- event-driven resident-helper waits;
- NVOF on its own command list with a GPU-side fence;
- `WaitForNextTick` on a high-resolution waitable timer (113% → 18% of one
  core);
- guides and guide passes gated on `GuidesRequired()`;
- `CoveredRanges()` memoised inside `NeuralSegmentIndex`;
- `/MP`.

**Tests and supply chain**
- **Tests are real, not smoke.**
  - A release-notes body with a planted `tag_name` forces a correct parser.
  - Byte-identical pixels are asserted across two seek paths.
  - Cache corruption is covered: tamper, interrupted staging, nonce
    collision, dead-owner sweep.
- **Everything is pinned and checked.** Every action is SHA-pinned.
  - Every download is hash-checked before it runs and again after staging.
  - Runtime archives are checked, then each extracted file and its
    Authenticode state.
  - The DLSS SDK is pinned to a commit.
  - The package has an exact allowlist and zip-bomb and traversal limits.
  - CMake refuses to ship a build with test seams compiled in.
- **Workflows:** minimal token permissions per job, no `pull_request_target`,
  inputs reach shell steps through `env`, and publishing runs in its own job
  with nothing checked out.
- **File handling:** drag-drop, file dialogs (all `OFN_NOCHANGEDIR`), the
  `--render` guard against overwriting its own source, the recent-media
  parser, and the atomic `ReShade.ini` write.
- **SHA-256 has exactly one implementation.**
- **The update check** uses `WINHTTP_FLAG_SECURE`, bounded timeouts, a
  body-size cap, and downgrade protection.
- **Zero /W4 warnings**, `#pragma once` everywhere, no `#if 0`, and no
  commented-out code.

`docs/TROUBLESHOOTING.md` quotes literal log lines and tells the user how to
read them. Keep that standard. The docs' menu paths and shortcuts match
`AppMenu.cpp` and `main.cpp`, and every repository path they cite exists.

---

## Method

**2026-09-25, against 13f92b7.** Five agents, read-only; the lead re-read the
top findings against source.

| Agent | Scope |
| --- | --- |
| Neural audit | Helper and IPC, resident loop, `OfflineNeuralRenderer` (in part), cache, runtime lock, DLSS backends |
| Media audit | Decode, playback, audio/WASAPI, D3D12 and TDR recovery, NVENC, export, subtitles, YouTube, update check; three findings reproduced on FFmpeg 9.0.1 |
| Shell and release audit | `main.cpp` shell and shutdown, CMake, workflows, packaging and fetch scripts, tests, licences; read-only `gh`, `git ls-remote`, `dumpbin` |
| Landscape research (two passes) | DLSS 5 projects and trackers, NVIDIA 2026 SDKs, community sentiment, ICAT, video-compare, Improve-ImgSLI, Topaz, FFMetrics |

**Not audited this time:** `TemporalGuides.cpp`, `OpticalFlowNvof.cpp`,
`DLSSGBackend.cpp`, most of `FrameGenerationPass.cpp`, and about half of
`OfflineNeuralRenderer.cpp`. The build and CTest baseline was not re-run.

The 2026-09-23 audit (23d71d9) used the same shape: playback, neural and
build/CI/docs audits plus pipeline and UX research. Its open items are carried
over above.
