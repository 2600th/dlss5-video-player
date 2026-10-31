# Improvement task list

_Audited against 0.26.0 (13f92b7) on 2026-09-25. Status re-checked against
0.26.2 (335edb3) on 2026-09-26: every item below is still open unless it says
what part is done. File:line citations are as of the audit and have drifted by
up to ~75 lines in `main.cpp`; search for the named symbol._

_Refreshed on 2026-10-16 against 0.28.2 (f42cb39) with a landscape survey
(`reports/landscape-refresh-2026-10-16.md`): P0.19, P1.35-P1.43, P2.43-P2.58
and P3.14-P3.20 are new, several open items gained evidence, and the work is
grouped into [milestones](#milestones)._

_Re-checked against 0.28.3 (4dec5c3) on 2026-10-16. M0 and M1 landed, but
several of their Test steps were not run, and two CHANGELOG claims were never
tried. Those gaps are P1.45-P1.48 (milestone M1.1), P2.59 and P2.60._

_Refreshed on 2026-10-31 against 0.28.3 (8dfded2) with a second landscape
survey (`reports/landscape-refresh-2026-10-31.md`). NVIDIA's October model is
obtainable, so P3.14 is unblocked and becomes milestone M1.2 under the
interrupt rule. M1.1, M1.2 and M2 ship together as 0.30.0, and M3 follows as
0.31.0. P0.20, P1.49-P1.51, P2.61-P2.66 and
P3.21 are new._

_Re-checked against 0.30.0 on 2026-10-31: M1.1, M1.2 and M2 landed, and
their tasks are removed. What they left open is P1.52-P1.54 and P2.67-P2.69,
and P3.5 keeps only its signed-build and other-generation half. P1.54, the
0.30.0 hands-on checks, opens M3._

Open work only, highest priority first. Completed items are removed rather
than ticked; what shipped is in `CHANGELOG.md` and git history. When a task
lands, delete it from this file in the same change.

**IDs are never reused.** Source comments cite them (`P2.8`, `P3.7`), so a
new task takes the next free number in its tier even when lower numbers are
free. Next free: **P0.21, P1.55, P2.70, P3.22**.

Sources: three code audits (neural helper/cache/runtime; decode, playback,
audio and export; app shell, build, CI and release) and two web surveys of the
DLSS 5 landscape, on 2026-09-25, plus the open items carried over from the
2026-09-23 list; the 2026-10-01 improvement-ideas survey and the 2026-10-04 UI
proposal (`reports/`, not committed); and the 2026-10-16 and 2026-10-31
landscape refreshes.

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

**Baseline.** Measured on 2026-09-30 (0.26.2 plus P1.24-P1.33 and
`dlss5-convert`) on an RTX 5090 with the RTX Video SDK staged: Release build
clean at **0 warnings /W4**; CTest **37 registrations, all pass** (18 portable,
19 hardware); `site/test.ps1` 61 pass at 0.26.2, not re-run.

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

| ID | Task | Effort | Impact | Milestone |
| --- | --- | :---: | --- | :---: |
| **P0** | **Fix before the next release** | | | |
| | _None open_ | | | |
| **P1** | **Next** | | | |
| [P1.54](#p154) | 0.30.0 hands-on checks | S | Player | M3 |
| [P1.52](#p152) | RenoDX 10.8.0 and 310.8.SF-v2 on RTX 20, 30 and 40, and the driver floor | S | Pipeline, Docs | M5 |
| [P1.53](#p153) | dlssg_for_sm86 0.4.x: a real load on an RTX 20 or 30 | S | Pipeline | M5 |
| [P1.44](#p144) | Reconcile the two brightness instruments | S | Pipeline | M3 |
| [P1.51](#p151) | Keep a DirectX dump file when the GPU hangs | S-M | Pipeline | M5 |
| [P1.50](#p150) | List the core zip on WinGet | S | Release | M6 |
| **P2** | **High-value features** | | | |
| | _Comparison and review_ | | | |
| [P2.31](#p231) | Loop In/Out, slow motion and a backward frame step | S-M | Player | M3 |
| [P2.29](#p229) | Flicker-map compare view | M | Player | M3 |
| [P2.30](#p230) | Per-frame metric lane and worst-frames navigator | M | Player, Pipeline | M3 |
| [P2.28](#p228) | Compare two to four neural presets on one frame | M | Player | M3 |
| [P2.61](#p261) | Face identity drift in the render report and the metric lane | M | Pipeline, Player | M3 |
| [P2.62](#p262) | A perceptual (FLIP) difference view | S-M | Player | M3 |
| [P2.27](#p227) | Save a comparison clip | S-M | Player, Pipeline | M6 |
| [P2.32](#p232) | Scopes and a pixel probe | M | Player | M6 |
| [P2.33](#p233) | Comparison frame pack | S-M | Player | M6 |
| [P2.34](#p234) | Several pinnable magnifiers | S-M | Player | M6 |
| | _Temporal and look_ | | | |
| [P2.43](#p243) | Temporal stability v2: steady the edit, timed by the frame | M-L | Pipeline, Player | M4 |
| [P2.36](#p236) | Opt-in face protection mask | M-L | Player, Pipeline | M4 |
| [P2.45](#p245) | Post-model look protections: tone range and band-split Mix | M | Player | M4 |
| [P2.37](#p237) | Opt-in: bake the Mix, mask and adjustments into exports | M | Pipeline | M4 |
| [P2.46](#p246) | Grain restore after the model | S-M | Player | M4 |
| [P2.66](#p266) | Per-scene presets on detected cuts (only if asked) | M | Player, Pipeline | M8 |
| | _Render and export_ | | | |
| [P2.59](#p259) | NVENC AQ at 4K with VMAF v1, through the direct session | S | Pipeline | M5 |
| [P2.60](#p260) | Re-time M1 on the RTX 4080 SUPER | S | Pipeline | M5 |
| [P2.35](#p235) | Resume, pause and join renders across sessions | M-L | Pipeline | M5 |
| [P2.47](#p247) | Pre-warm the helper; remember the preflight verdict | M | Pipeline | M5 |
| [P2.48](#p248) | Background renders yield to playback and other apps | M | Pipeline | M5 |
| [P2.49](#p249) | Choose the GPU for the neural render | M | Pipeline, Player | M5 |
| [P2.51](#p251) | Deinterlace interlaced sources before the model | M | Pipeline | M5 |
| [P2.53](#p253) | Say a long job finished, outside the window | S | Player | M5 |
| [P2.64](#p264) | Measure, then reuse exact-duplicate source frames | S-M | Pipeline | M5 |
| [P2.68](#p268) | One cache entry for the previous runtime's two networks | S | Pipeline | M5 |
| [P2.65](#p265) | Say in the exported file how it was made | S | Pipeline | M6 |
| [P2.40](#p240) | Opt-in subtitle burn-in for MP4 and GIF | M | Pipeline | M6 |
| [P2.38](#p238) | Measure DLSS SR presets L and M | S | Pipeline | M8 |
| [P2.39](#p239) | RTX VSR on the neural render | M | Player, Pipeline | M8 |
| | _Interface, input and convenience_ | | | |
| [P2.67](#p267) | A native speaker's review of the Simplified Chinese text; more languages | S | Player, Docs | M8 |
| | _HDR_ | | | |
| [P2.56](#p256) | Try an FP16 scRGB swap chain for HDR presentation | S-M | Player | M7 |
| | _Tests_ | | | |
| [P2.69](#p269) | A YouTube resolver test that fails under load | S | Release | M3 |
| | _Carried over_ | | | |
| [P2.4](#p24) | Evaluate Video Depth Anything through the guide harness | M-L | Pipeline | M8 |
| **P3** | **Strategic or large** | | | |
| [P3.9](#p39) | HDR output of the render | L | Pipeline | M7 |
| [P3.18](#p318) | A measured HDR→SDR peak for the model's input | M | Pipeline | M7 |
| [P3.15](#p315) | NVIDIA Video Effects SDK: frame generation and VSR modes | M-L | Pipeline | M8 |
| [P3.16](#p316) | Frame Generation "Performance mode" as a labelled rung | S | Pipeline | M8 |
| [P3.17](#p317) | A JavaScript runtime for yt-dlp after Deno | S | Release | M8 |
| [P3.19](#p319) | Overlap the per-frame stages | L | Pipeline | M8 |
| [P3.20](#p320) | A residual-composite reduced-scale rung | M | Pipeline | M8 |
| [P3.10](#p310) | Processing scale above 100% as an export-only rung | M | Pipeline | M8 |
| [P3.11](#p311) | Compare against an external file | M-L | Player | M8 |
| [P3.12](#p312) | Network sources beyond YouTube | M | Player | M8 |
| [P3.13](#p313) | Render queue (owner's decision) | M | Pipeline | M8 |
| [P3.5](#p35) | NVIDIA's signed DLSS-NR runtime, and 310.9.16 beyond RTX 50 | M | Pipeline, Release | M8 |
| [P3.21](#p321) | Opt-in C2PA content credentials on exports | M | Pipeline, Release | M8 |
| [P3.6](#p36) | Neural optical flow as an export-only rung | M-L | Pipeline | M8 |
| [P3.8](#p38) | Repository media hygiene | S | Release | M8 |

---

<a id="milestones"></a>
## Milestones

Grouped by release, by what each group needs first, and by code area, so one
branch touches one part of the tree. Each milestone is one minor release
unless it says otherwise. Effort: S is half a day to a day, M two to five days,
L one to two weeks.

| | Release | Theme | Tasks | Size | Needs first |
| --- | --- | --- | --- | :---: | --- |
| **M0, M1** | 0.28.3 | Shipped on 2026-10-16: YouTube, provenance, runtime readiness, seven measurements | - | - | - |
| **M1.1, M1.2, M2** | 0.30.0 | Shipped on 2026-10-31: 0.28.3's claims proved or corrected, a stable yt-dlp, DLSS-NR 310.9.16 on RTX 50 with two networks, RenoDX 10.8.0, the UI restructure, translatable | - | - | - |
| **M3** | 0.31.0 | Judge it: the review loop | P1.54 (first), P2.31, P2.29, P2.30, P2.28, P1.44, P2.61, P2.62, P2.69 | ~3 weeks | P1.54; M2's compare-bar slots are in 0.30.0 |
| **M4** | 0.32.0 | Fix it: faces, flicker and colour | P2.43, P2.36, P2.45, P2.37, P2.46 | ~3 weeks | M3 (tools to judge it); M2's Picture panel slot is in 0.30.0 |
| **M5** | 0.33.0 | Never lose work, never wait | P2.35, P2.47, P2.48, P2.49, P2.51, P2.53, P2.59, P2.60, P2.64, P2.68, P1.51, P1.52, P1.53 | ~3 weeks | runs beside M3-M4; RTX 20-40 cards for P1.52 and P1.53 |
| **M6** | 0.34.0 | Share and inspect | P2.27, P2.33, P2.32, P2.34, P2.40, P2.65, P1.50 | ~2 weeks | M3 (worst frames feed the frame pack) |
| **M7** | 0.35.0 | HDR end to end | P3.9, P3.18, P2.56 | ~3 weeks | - |
| **M8** | - | Gated or strategic | P3.15, P3.16, P3.17, P3.19, P3.20, P2.38, P2.39, P2.4, P2.66, P2.67, P3.5, P3.6, P3.8, P3.10-P3.13, P3.21 | - | each item's blocker |

**M0 and M1 shipped together as 0.28.3** (2026-10-16), a patch because nothing
in them changes a rendered pixel. The measurements decided four later designs.
The control mask is not reachable, so M4's protections are post-model blends.
AQ loses, so M5's encode default stays. The NV12 transport is small, so the
zero-copy line stays parked. Stacked passes drift both ways, so there is no
correction. RenoDX 8.5 is adoptable with three config changes (P3.14). Owner
actions still open: close #15, fixed in 0.28.2 and answered on 10-04; decide on
#3, which has waited on its reporter since 09-09.

**M1.1, M1.2 and M2 shipped together as 0.30.0** (2026-10-31), one release
where there would have been three. M1.1 proved or corrected 0.28.3's claims,
and its CHANGELOG lists the corrections. M1.2 brought the October model, with
four decisions that still hold
([runtime-310916-20261031](measurements/runtime-310916-20261031/REPORT.md)).
Network A is the default: it renders the same frames as 0.28.3, and B is an
opt-in rung. 310.9.16 runs only on RTX 50, the only generation measured; other
GPUs keep 310.8.SF-v2. The player pins `NRDetailStability=1` and
`NRSkinIndependent=0`, which keep RenoDX 10.8.0 within 0.1 level of 6.5.3. It
writes colour strength 0 as 0.01, because 10.8.0 refuses a session with every
pass at zero. M2 restructured the UI and made it translatable; a Simplified
Chinese file ships unreviewed (P2.67). What 0.30.0 left untested by hand is
P1.54, and what needs other cards is P1.52 and P1.53.

**M2 before M3 and M4.** The UI proposal's restructure made the places the
next controls go, so users relearned once and the docs were rewritten once.
M2 left marked slots for M3 in the Compare menu (`AppMenu.cpp`) and before `⋯`
on the compare bar, for the loop, speed and flicker map. It left one for M4
under Tint in the Picture panel's viewing zone, for the protections and grain.
The 2026-10-31 refresh briefly put M3 first, because Visual Enhancer v20.0
(2026-10-30) shipped split, cycle, a difference heatmap and saved sessions.
The owner kept M2 first. M3 is what remains the player's alone: the review
loop on a progressive render. It ships next, as 0.31.0.

**M5 runs beside M3 and M4.** It touches the worker, cache, decode child and
NVENC; M3 and M4 touch the player shell, the compare views and the present
path. Separate worktrees, separate branches.

**The quality rule still decides defaults.** M4, P2.51 and P3.18 change
pixels: each is opt-in, or on by default only where it moves quality up
(deinterlacing a flagged-interlaced source, an encode that wins at matched
size), each is a cache- or export-key term, and each is scored with P1.42.

---
---

# P0 — Fix before the next release

P0.11-P0.18 landed on 2026-09-25 (branch `fix/p0-all`). P0.19 landed in 0.28.3
(2026-10-16): the yt-dlp nightly 2026.10.10.233141, pinned by SHA-256. P0.20
landed in 0.30.0 (2026-10-31): yt-dlp 2026.10.26, a stable release past
certifi's critical certificate fix, pinned by SHA-256. No P0 task is open.

---

# P1 — Next

P1.24-P1.34 landed on 2026-09-30 (branch `fix/p1-all-and-cli`), with the
`dlss5-convert` command-line converter. The release-tag protection P1.30 asked
for is repository ruleset 24245175: `refs/tags/dlss5-video-player-v*` cannot be
updated or deleted.

P1.35-P1.40, P1.42 and P1.43 landed in 0.28.3 (2026-10-16). Two parts stay
with the owner. One is asking the DLSS5 add-on's distributor (RankFTW/rhi-repo,
or ShortFuse) for its source and licence; `THIRD_PARTY.md` says what was found
until then. The other is Authenticode signing (cost, identity). P1.38 reads
NGX's `DLSSNR.*` floor but uses it only to explain a failed probe, never to
refuse a setup the probe accepts, since NGX's floor sits above the 610.47 the
runtime was measured working on. P1.40 recognises 0.4.0 and names a lossy
`Optimized`; the one-click pin stays at 0.3.5, which already has the
re-creation fix.

P1.41 was measured in the same release and needs no correction
([passes-drift-20261016](measurements/passes-drift-20261016/REPORT.md)). From one
to four passes the mean luma moves −1.10 to +1.74 codes, in both directions;
each pass compounds the model's own change, faces cool, and flicker rises.

P1.45-P1.49 landed in 0.30.0 (2026-10-31), milestone M1.1. Each proved its
claim or corrected it in 0.30.0's CHANGELOG. One step stays with the owner:
reading `powercfg /requests` during a job needs an elevated prompt, so that
claim is still unverified. The keep-awake request is now tested to be held for
exactly the job's length, and a request that cannot be made is logged. Read
`powercfg` in the same sitting as P1.54. P1.46's real load of a 0.4 build is
P1.53.

<a id="p154"></a>
### P1.54 · 0.30.0 hands-on checks

`S` · **Player** · _the first thing in M3; needs a person at the machine_

- **Problem.** 0.30.0's new player features have unit tests and smokes, but
  several were never tried by hand. M1.1 showed what that costs: 0.28.3's
  trailers were resolved but never played, and past a minute the sound
  stopped. Not yet tried in 0.30.0:
  - a real clipboard copy and paste;
  - a real resume seek, including YouTube's, cached neural playback and a
    rendered range;
  - YouTube captions drawn on screen;
  - a real double-click on Mix;
  - the pop-up menus, `Shift+F10`, and the `input.conf` re-read when the
    window is activated;
  - the toolbar's look at each DPI.
- **Where.** The packaged 0.30.0 player. `main.cpp`, `AppMenu.cpp`,
  `RecentMedia.cpp`, `YouTubeResolver.cpp`, `SubtitleOverlay.cpp`,
  `UiLayout.cpp`.
- **Fix.** In the package, by hand:
  - `Ctrl+Shift+C`, then paste into another app. `Ctrl+V` with a file copied
    in Explorer, a YouTube link, a file path and a picture; a link to another
    site must be refused.
  - Stop a local video, a YouTube video, a video in cached neural playback and
    one inside a rendered range partway. Reopen each: it opens where it
    stopped, and the toast says where.
  - Turn on a written and an auto-generated track under **Playback >
    Subtitles** and watch them drawn.
  - Double-click Mix in the Picture panel and check the result against
    `docs/USAGE.md`.
  - Open the picture menu by right-click, `Shift+F10` and the menu key. Edit
    `input.conf` while the player is in the background, switch back, and use
    the new key.
  - Look at the toolbar at 100 % and at each higher display scale.

  Anything that fails is fixed in the next patch and named in its CHANGELOG.
- **Test.** The notes in the PR, with screenshots of the captions and of the
  toolbar at each scale.

<a id="p152"></a>
### P1.52 · RenoDX 10.8.0 and 310.8.SF-v2 on RTX 20, 30 and 40, and the driver floor

`S` · **Pipeline, Docs** · _every user without an RTX 50 gets this pair; needs the cards_

- **Problem.** 0.30.0 moves every GPU to RenoDX 10.8.0, and RTX 20, 30 and 40
  run it over 310.8.SF-v2. No RTX 20, 30 or 40 card has run that pair. The two
  pinned keys (`NRDetailStability=1`, `NRSkinIndependent=0`) keep it within
  0.1 level of 6.5.3, but that was measured only on an RTX 5090
  ([runtime-310916-20261031](measurements/runtime-310916-20261031/REPORT.md)).
  Both NR DLLs also declare a minimum driver of 615.00. The docs say 610.47,
  the lowest driver the project has rendered on, on an RTX 4080 SUPER with
  310.8. Nobody has rendered with 10.8.0 on a driver below 615.00.
- **Where.** `tools/benchmark/m12.py`, the runtime smokes,
  `RuntimePolicy.h` (`kNeuralDriverFloor`), the docs that name 610.47
  (README, `docs/DLSS5_SETUP.md`, `docs/TROUBLESHOOTING.md`,
  `docs/USAGE.md`), `docs/measurements/`.
- **Fix.** On an RTX 40 card and on an RTX 20 or 30 card, run every runtime
  smoke, and `m12.py`'s comparison of 10.8.0 with the pinned keys against
  6.5.3 on the five clips. On the RTX 4080 SUPER, render with 10.8.0 on
  610.47. If it renders, the docs keep 610.47 and say that the DLLs' 615.00
  did not stop it. If it does not, the documented floor and
  `kNeuralDriverFloor` rise to the lowest driver that renders, and the
  CHANGELOG says so. If the picture moves more than 0.1 level on a card, find
  out why before the next release.
- **Test.** A measurement report with the distance per clip and card, the
  smoke logs, and the 610.47 render log.

<a id="p153"></a>
### P1.53 · dlssg_for_sm86 0.4.x: a real load on an RTX 20 or 30

`S` · **Pipeline** · _what P1.46 left; needs an RTX 20 or 30_

- **Problem.** 0.30.0 tests how 0.4.0 and 0.4.1's settings are read, and
  recognises 0.4.1. No real 0.4.0 or 0.4.1 has been loaded on an RTX 20 or 30,
  and the CHANGELOG says so. 0.4.0 crashes on some older drivers
  ([#640](https://github.com/sdli1995/dlssg_for_sm86/issues/640),
  [#642](https://github.com/sdli1995/dlssg_for_sm86/issues/642)), and 0.4.1
  makes R580 a hard floor.
- **Where.** `FrameGenAddon.h`, the frame generation smokes.
- **Fix.** Download each build into a new empty folder, check it against the
  release's digest, install it beside a build, and run `DlssgProbeSmoke`,
  `DlssgEvaluateSmoke` and `FrameGenerationSmoke` on an RTX 20 or 30 card. It
  must load, and the start screen and log must name a lossy `Optimized` and
  `Engine=XeSS` as they do in the tests. If a build does not load, the next
  CHANGELOG and `docs/TROUBLESHOOTING.md` say so. The one-click pin stays on
  0.3.5.
- **Test.** The smoke logs from both real loads.

<a id="p144"></a>
### P1.44 · Reconcile the two brightness instruments

`S` · **Pipeline** · _measurement infrastructure_

- **Problem.** On the same five renders, the mean of the Y plane puts one pass
  0.6-1.5 codes darker than the source. The worker's own `luma_shift` puts it
  about 1.2 codes brighter
  ([passes-drift-20261016](measurements/passes-drift-20261016/REPORT.md)). The
  pass-to-pass drift agrees within 0.3 codes, so one instrument has an offset:
  sampled cells, range, or a transfer step. P2.30's metric lane and the
  render report would show whichever is wrong.
- **Where.** `TemporalMetrics.h`, `tools/benchmark/m1.py`, `analyze.py`.
- **Fix.** Score one Lossless render both ways on the same frames, find the
  step that differs, and fix the instrument that is wrong. While there, add the
  sixth clip and an unchained run, which passes-drift-20261016 left out.
- **Test.** Both agree within 0.1 code on the passes-drift clips.

<a id="p151"></a>
### P1.51 · Keep a DirectX dump file when the GPU hangs

`S-M` · **Pipeline** · _diagnostics; local only_

- **Problem.** A TDR or hang during a render leaves DRED breadcrumbs in the log
  and a CPU minidump (`CrashDump.h`), but no GPU-side capture. DLSS 5 tools see
  device resets on RTX 20-40
  ([NeuralScreen #181](https://github.com/perseval-BLR/NeuralScreen/issues/181),
  `0x887A0007`).
- **Where.** `CMakeLists.txt` and packaging (`D3D12Core.dll` and the
  `D3D12SDKVersion`/`D3D12SDKPath` exports), `D3D12Renderer.cpp`, the worker's
  device creation, `CrashDump.h`, `docs/TROUBLESHOOTING.md`.
- **Fix.** [Agility SDK 1.620](https://devblogs.microsoft.com/directx/agilitysdk-1-620-retail-and-1-722-preview/)
  (2026-10-28) ships
  [DirectX Dump Files](https://devblogs.microsoft.com/directx/directx-dump-files-are-now-released/).
  On R620+ drivers and Windows 11 build 26100/26200.8524 or later, a hang
  writes a `.dxdmp`, and the app gets the path through a callback. Keep the
  file beside the crash dump, name it in the log, and never upload it. The
  player uses the OS D3D12 runtime today and ships no Agility SDK. Adopting
  one is a dependency change of its own: pinned and hash-checked like the
  rest, on the package allowlist, and kept out of `neural-runtime/` unless the
  runtime lock is re-measured with it.
- **Test.** The GPU smokes on the Agility runtime. Check whether
  `UpscalingGpuSmoke`'s `RemoveDevice` case leaves a `.dxdmp`. If it does not
  (dumps are for hangs and TDRs), add a test-seam shader that spins
  past the TDR timeout, built only into test builds. On an unsupported setup the log says why there
  is none.

<a id="p150"></a>
### P1.50 · List the core zip on WinGet

`S` · **Release** · _trust; the owner submits the manifest_

- **Problem.** Impostor re-uploads keep multiplying. Between 2026-10-25 and
  10-28, six org-named SEO accounts released zips under DLSS 5 tool names. A
  lure posing as DLSS5-One-Click tells users to run a PowerShell `iex(irm …)`
  line
  ([rogeriosanguino956/dlss5-settings-manager](https://github.com/rogeriosanguino956/dlss5-settings-manager),
  10-29). The player is offered only from its release page. mpv, MPC-HC, VLC
  and Upscayl are on WinGet; no DLSS 5 tool is.
- **Where.** A `microsoft/winget-pkgs` manifest, `packaging/`, README, `site/`.
- **Fix.** List the core zip only, pinned by SHA-256, since the complete zip's
  runtime terms are unresolved (`THIRD_PARTY.md`). Generate the manifest from
  the release script so every release updates it. Add one line to the README's
  download section and the site: the player is never installed by running a
  PowerShell command, and anything that says so is not this project.
- **Test.** `winget validate` on the generated manifest; a `winget install`
  into a clean user profile.

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

P2.52, P2.41, P2.42, P2.54, P2.55 and P2.63 landed in 0.30.0 (2026-10-31),
milestone M2, with the UI proposal's compare-bar controls (X6), the pipeline
chip (X10) and the Picture panel's marks and resets (X2's remainders).

## Comparison and review

<a id="p227"></a>
### P2.27 · Save a comparison clip

`S-M` · **Player, Pipeline**

- **What.** Split, wipe, side by side or 2x2 as a labelled MP4 over In/Out,
  built from the cached render plus the source (`hstack`/`overlay`/`drawtext`),
  plus a 9:16 stacked layout, since "off vs on" clips are mostly shared as
  Shorts.
- **Why.** "Off vs on" clips are how DLSS 5 is judged
  ([TechRadar](https://www.techradar.com/computing/nvidias-dlss-5-is-going-viral-for-all-the-wrong-reasons-here-are-the-5-most-controversial-examples-of-the-ai-powered-breakthrough-in-action)).
  The player saves comparison stills only; Visual Enhancer's 2-Up is preview
  only.
- **Where.** `ExportPipeline.h`, `MediaPipeline.cpp`, `AppMenu.cpp`,
  `main.cpp`, `RenderCommandLine.h`.

<a id="p228"></a>
### P2.28 · Compare two to four neural presets on one frame

`M` · **Player**

- **What.** Preset A vs preset B in two panes with shared zoom, on the paused
  frame first; pane B comes from a second settings key in the cache. Then up
  to four "finalists" in the 2 × 2, a short marked range so flicker differences
  show, and "Keep this one", which makes the choice the video's setting.
- **Why.** Choosing a preset today means re-rendering and remembering.
  DLSS5-Image-Converter ships "Compare styles"; Improve-ImgSLI's master adds
  "Keep best / Compare finalists"
  ([37b2bfd](https://github.com/Loganavter/Improve-ImgSLI/commit/37b2bfddb0db5046496588f12561adee174b626c)).
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
### P2.31 · Loop In/Out, slow motion and a backward frame step

`S-M` · **Player**

- **What.** Loop the marked range; play at 1/2, 1/4 and 1/8 speed on the
  exact cached frames (no interpolation), audio muted; and step back a frame
  with `,`, as mpv does. Only `.` steps today (`StepCachedFrame` in
  `main.cpp`); on a rendered range the cache makes a back-step nearly free.
- **Why.** Temporal artefacts are judged by looping a short range slowly.
  ICAT and [video-compare](https://github.com/pixop/video-compare) both do it;
  nothing in `src/` does.
- **Where.** `RangeSelection.cpp`, `PlaybackCadence.h`,
  `PlaybackTickPolicy.h`, `SynchronizedPlayback.cpp`, `AppMenu.cpp`.

<a id="p232"></a>
### P2.32 · Scopes and a pixel probe

`M` · **Player**

- **What.** Waveform, vectorscope and histogram for original vs render, and a
  pixel probe showing both values under the cursor. RGB and YUV modes, and
  legal-range shading, which video-compare declined because FFmpeg's histogram
  filter cannot draw it
  ([#154](https://github.com/pixop/video-compare/issues/154), 2026-10-13).
- **Why.** Makes the colour-shift complaints measurable ("washed out",
  saturation loss, pink lips). video-compare has them; `src/` has none.
- **Where.** `D3D12Renderer.cpp`, `UiLayout.cpp`, `AppMenu.cpp`.

<a id="p233"></a>
### P2.33 · Comparison frame pack

`S-M` · **Player**

- **What.** N frames (at markers, random, or the worst flicker) saved as
  source and render PNG pairs in the slow.pics layout, with a local HTML
  viewer. Local only; upload only on explicit request. Automatic picks keep a
  minimum spacing and skip black or solid frames, as
  [Pear v1.1.0](https://github.com/rlaphoenix/Pear/releases/tag/v1.1.0) does
  (2026-10-07).
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

<a id="p261"></a>
### P2.61 · Face identity drift in the render report and the metric lane

`M` · **Pipeline, Player** · _a measurement; changes no pixel_

- **What.** For each face track: ArcFace cosine similarity between the source
  face and the rendered face, plus a frame-to-frame identity distance
  ([VIDD, arXiv 2404.19500](https://arxiv.org/abs/2404.19500)). The worst
  values feed P2.30's lane and its worst frames, and the render report gains
  a "face identity" line beside flicker, grain and colour shift.
- **Why.** The loudest public complaint about DLSS 5 is that faces change.
  P2.36 protects faces; nothing measures how much they changed. No DLSS 5 tool
  measures it (2026-10-31 survey).
- **Where.** A face detector shared with P2.36, a small ONNX model run in a
  child process (TensorRT-RTX warns against DirectML in the same process as
  the runtime), `TemporalMetrics.h`, `NeuralReceipt.cpp`, `TimelinePolicy.h`.
- **Risk.** The papers call it a relative signal that is easy to game. Report
  it as a relative signal, never as "identity preserved". Score it against
  frozen output (P1.42) before trusting a low number. The model adds to the
  package, so check its licence; YuNet is MIT, and ArcFace weights vary.

<a id="p262"></a>
### P2.62 · A perceptual (FLIP) difference view

`S-M` · **Player** · _presentation only_

- **What.** A compare view beside Difference: NVIDIA's
  [FLIP](https://github.com/NVlabs/flip) error map (BSD-3), one compute pass
  over the original and the render at the displayed frame.
- **Why.** Difference shows how much changed, amplified. FLIP shows whether a
  viewer would see it at this viewing distance, which is the question behind
  "does it actually look better". NVIDIA's ICAT pairs its views with FLIP.
  Visual Enhancer v20.0 (2026-10-30) has a difference heatmap; its release
  notes do not say it is perceptual.
- **Where.** `CompareViewPolicy.h`, `D3D12Renderer.cpp`, a new FLIP shader,
  `AppMenu.cpp`, the comparison image's footer (name the view and its
  viewing-distance setting).

## Temporal and look

Faces moving, flicker and colour shift are still the complaints that make
people abandon a tool's version
([VE #95](https://github.com/Merserk/dlss5-visual-enhancer/issues/95),
[NeuralScreen #151](https://github.com/perseval-BLR/NeuralScreen/issues/151),
[#160](https://github.com/perseval-BLR/NeuralScreen/issues/160)). Two things
changed on 2026-10-08: the runtime's own control mask became usable in a
shipping tool, and "steady the edit, not the frame" became the common
stabiliser design. Measure first (M1), then build (M4).

M1's verdicts (0.28.3): the runtime's control mask cannot be reached through
either RenoDX ([control-mask-20261016](measurements/control-mask-20261016/REPORT.md)),
so every protection below is a blend after the model. NVIDIA's RTX Remix
defaults land at Gentle's strength
([nvidia-defaults-20261016](measurements/nvidia-defaults-20261016/REPORT.md));
see Parked.

<a id="p243"></a>
### P2.43 · Temporal stability v2: steady the edit, timed by the frame

`M-L` · **Pipeline, Player** · _new rungs; off by default until they win P1.42's yardstick_

- **What.** Three changes to the Temporal stability pass:
  1. **History by frame time.** `w' = w^(Δt/Δt_ref)`, so a rung is the same
     time constant at 24p and 60p; `TemporalStabilityPolicy.h` has no frame
     duration today.
     [NeuralScreen v2.4.0](https://github.com/perseval-BLR/NeuralScreen/releases/tag/v2.4.0)
     fixed shading trailing on moving faces this way.
  2. **Edit-only mode.** Warp and blend `render − source` along motion and add
     it to the current source, so source detail is never delayed.
     [NeuralScreen v2.2.0](https://github.com/perseval-BLR/NeuralScreen/releases/tag/v2.2.0):
     edit instability down 43-49 %, strength within 2 %, 0.1-0.3 ms a frame.
     The Resolve plugin's anti-flicker
     ([0.4.1](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5/releases/tag/v0.4.1))
     and [Dlss5VideoEnc](https://github.com/Johnwikix/Dlss5VideoEnc) do the
     same.
  3. **Trust motion only where it beats none.** Keep an NVOFA vector only where
     it predicts better than zero motion (NeuralScreen: "jelly" on still
     background beside a moving object, 91.7 % to 0 %), and judge cuts on
     aligned frames (false resets 4.8 % to 0.6 %).
- **Why.** The shipped pass blends the whole frame and "costs some fine
  detail" (`docs/USAGE.md`).
- **Where.** `TemporalStabilityShader.h`, `TemporalStabilityPolicy.h`,
  `TemporalSettings.h`, `FlowGate.h`, `SceneCut.h`, `tools/benchmark/cutlab.py`.
- **Risk.** Change 1 moves the pixels of existing stability renders, so it is a
  cache-key term for those settings. Measure on faces specifically.
- **Note.** RenoDX 8.5's Detail stability, on by default, is change 2 inside
  the add-on: a filter on the model's edit along the motion vectors
  ([renodx-85-20261016](measurements/renodx-85-20261016/REPORT.md)). Score it
  with P1.42's yardstick before building the player's own.

<a id="p245"></a>
### P2.45 · Post-model look protections: tone range and band-split Mix

`M` · **Player** · _opt-in, instant like Mix; exported only through P2.37_

- **What.** Two controls beside Mix: a tone-range mask that keeps the source in
  the shadows or highlights, and a band-split Mix with two directions, "keep
  source tone" (the source's low frequencies and chroma under the model's
  detail) and "keep source detail" (the reverse), gated by the existing
  source-alignment test so copied edges do not ghost.
- **Why.** "DLSS 5 destroys pronounced shadows", and sliders did not fix it
  ([VE #85](https://github.com/Merserk/dlss5-visual-enhancer/issues/85)). The
  Resolve plugin ([0.4.0](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5/releases/tag/v0.4.0):
  hue, shadow and highlight protection), Veyra 2.0.4 and the OBS filter now all
  protect on the correction. [Hi-Light](https://arxiv.org/abs/2601.23167)
  (Apache-2.0 code) gives the band-split recipe and its ghosting warning.
- **Where.** `D3D12Renderer.cpp`, `CompareMaskPolicy.h`, `SliderPolicy.h`, the
  Image adjustments dialog in `main.cpp`.
- **Note.** The in-model mask is not reachable through RenoDX
  ([control-mask-20261016](measurements/control-mask-20261016/REPORT.md)), so
  both controls are post-model.

<a id="p246"></a>
### P2.46 · Grain restore after the model

`S-M` · **Player** · _opt-in, instant; lower value_

- **What.** Seeded, animated film grain added back after the model, sized from
  the grain loss the render report already measures.
- **Why.** [Visual Enhancer v14.0](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v14.0)
  added a grain stage (amount, size, colour, tonal response, seed).
- **Where.** `D3D12Renderer.cpp`, `TemporalMetrics.h`, `NeuralReceipt.cpp`.

<a id="p236"></a>
### P2.36 · Opt-in face protection mask

`M-L` · **Player, Pipeline** · _off by default; a blend after the model_

- **What.** Faces from Windows' built-in
  [`FaceDetector`](https://learn.microsoft.com/en-us/uwp/api/windows.media.faceanalysis.facedetector)
  (no model download) feed a mask frame by frame; the render report gains a
  face-region colour shift. The mask feeds the existing Mix mask after the
  model. The runtime's own control mask is not reachable through RenoDX
  ([control-mask-20261016](measurements/control-mask-20261016/REPORT.md)); an
  upstream change that forwards `DLSSNR.ControlMask` would reopen that route.
- **Why.** Missing masks are the cited cause of identity changes
  ([Held Games](https://heldgames.com/guides/dlss-5-official-vs-mod)); Visual
  Enhancer shipped Face/Skin Protection and replaced it with the control mask
  in v15; the player's own tooltip says Skin structure "is not face
  protection" (`Localization.h`).
- **Where.** A new face-mask policy and detector, `CompareMaskPolicy.h`,
  `D3D12Renderer.cpp`, `TemporalMetrics.h`, `NeuralReceipt.cpp`.
- **Risk.** Misses profile and small faces, so the mask pops; needs a hold and
  feather over time. If `FaceDetector` misses too many,
  [YuNet 2026may](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet)
  (MIT) is the fallback, at the cost of ONNX Runtime in the package.
- **Since 2026-10-31.** The cost is known: Veyra 2.1 ships face
  original-picture protection with YuNet, a 468-point MediaPipe mesh and
  One-Euro smoothing at 0.93 ms a frame at 1080p and 1.42 ms at 4K
  ([Veyra](https://github.com/Likely7/Veyra-NRVideo/releases), author's
  numbers). NVIDIA's dxvk-remix now always passes `pInControlMask`, with
  `InMaskBlendMode=Min` under the auto mask
  ([09808cc](https://github.com/NVIDIAGameWorks/dxvk-remix/commit/09808cc)).
  If RenoDX 10.8.0 forwards it (0.30.0's runtime report does not say),
  protection can happen inside the model rather than as a blend after it. The face tracks are shared with P2.61.

<a id="p237"></a>
### P2.37 · Opt-in: bake the Mix, mask and adjustments into exports

`M` · **Pipeline**

- **What.** Apply the Mix, the mask, the P2.45 protections and the image
  adjustments to "Save converted video" (e.g. `maskedmerge`), recorded in the
  receipt.
- **Why.** Today they affect only what you watch (`docs/USAGE.md`), and since
  0.28.2 the export dialog says so. The Resolve filter exposes Output Mix; it is
  the direct answer to "too much" on faces.
- **Where.** `ExportPipeline.h`, `MediaPipeline.cpp`, `CompareMaskPolicy.h`.

<a id="p266"></a>
### P2.66 · Per-scene presets on detected cuts (only if asked)

`M` · **Player, Pipeline** · _user-chosen, never automatic_

- **What.** Split the timeline at the cuts `SceneCut.h` already finds, and let
  the user give a range its own neural preset, for example Gentle on a face
  close-up and Standard on a landscape. Each range keeps its own settings key
  in the cache, and the export joins them.
- **Why.** One preset rarely suits every shot. Topaz Astra is reported to
  detect scenes and set per-scene settings (unverified, a third-party blog). It
  is not the Parked "auto model recommendation": there is one model, and the
  user chooses.
- **Where.** `SceneCut.h`, `NeuralPresets.h`, `NeuralSegmentIndex`,
  `TimelinePolicy.h`, `ExportPipeline.h`.
- **Gate.** Build it after P2.28, which is how a user would decide what each
  range needs, and only if users ask for it.

## Render and export

<a id="p235"></a>
### P2.35 · Resume, pause and join renders across sessions

`M-L` · **Pipeline** · _quality-neutral_

- **What.** Treat an earlier or interrupted run's segments as coverage when a
  video is reopened, and join partial renders, instead of rendering again.
  Pause a render or export and release the GPU, resume from the last committed
  segment, and pause safely when Windows suspends (`PBT_APMSUSPEND`).
- **Why.** Both are stated limits in the README. Losing long renders is the
  category's loudest complaint
  ([Topaz: 17 hours lost](https://community.topazlabs.com/t/pause-resume-threw-away-17-hours-of-rendered-footage/83266)),
  and [Visual Enhancer v15.0](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v15.0)
  now ships all three parts. `NeuralSegmentIndex` already tags segments by run
  and merges runs.
- **Where.** `NeuralCache.cpp`, `NeuralSegmentIndex.h`,
  `OfflineNeuralRenderer.cpp`, `main.cpp` (power notifications).

<a id="p247"></a>
### P2.47 · Pre-warm the helper; remember the preflight verdict

`M` · **Pipeline** · _quality-neutral_

- **What.** Start the preflight and priming speculatively when a file opens or
  a link resolves; cache the preflight verdict per runtime-lock digest, driver
  version and adapter LUID; skip pre-warming on battery.
- **Why.** The first `D` takes 7.9-9.0 s to the first rendered frame, 3.8 s of
  it the preflight probe (`CHANGELOG.md`, `docs/VERIFICATION-matrix.md`), and
  the first press is when a new user judges the tool. An idle helper holds
  about 1,061 MiB of VRAM, hence the battery rule. RenoDX arms only after its
  120 priming frames, so the saving may be the probe alone.
- **Where.** `NeuralPreflight.cpp`, `ResidentHelperPolicy.h`, `WarmUpPolicy.h`,
  `NeuralWorker.cpp`.

<a id="p248"></a>
### P2.48 · Background renders yield to playback and other apps

`M` · **Pipeline** · _quality-neutral_

- **What.** Lower GPU priority for the helper's work when it renders away from
  the playhead (`IDXGIDevice::SetGPUThreadPriority` or D3D12 queue priority),
  EcoQoS and below-normal process priority for background-only work, and a
  setting for it.
- **Why.** NeuralScreen measured NR falling from 54.1 to about 1.1 fps when a
  GPU-heavy app held the foreground
  ([#142](https://github.com/perseval-BLR/NeuralScreen/issues/142)), and its
  users now ask for process priority
  ([#170](https://github.com/perseval-BLR/NeuralScreen/issues/170)). A player
  rendering behind its own playhead creates that contention itself.
- **Where.** `NeuralWorker.cpp`, `NeuralWorkerMain.cpp`, `LiveSessionPolicy.h`,
  `RenderPacePolicy.h`.

<a id="p249"></a>
### P2.49 · Choose the GPU for the neural render

`M` · **Pipeline, Player**

- **What.** A neural-GPU picker by adapter LUID, and optionally a second GPU
  for whole background renders. Splitting one render across GPUs stays Parked.
- **Why.** Multi-GPU threads are a steady class
  ([Veyra #35](https://github.com/Likely7/Veyra-NRVideo/issues/35),
  [NeuralScreen #157](https://github.com/perseval-BLR/NeuralScreen/issues/157),
  [Neural Coprocessor #52](https://github.com/maohgad-web/Neural-coprocessor/issues/52),
  19 comments), and Visual Enhancer's most-discussed thread was a 10x slowdown
  fixed by choosing the GPU. The Parked list already prefers separate GPU
  choices (`GpuPreference.cpp`).
- **Where.** `GpuPreference.cpp`, `NeuralWorker.cpp`, `NeuralCache.cpp`,
  `AppMenu.cpp`. Confirm the cache key separates renders by adapter.
- **Since 2026-10-31.** It is now expected. Visual Enhancer v18 added GPU
  selection, and DLSS5Tool v3.1 fixed compositing that ignored the selected
  GPU on multi-GPU machines.

<a id="p259"></a>
### P2.59 · NVENC AQ at 4K with VMAF v1, through the direct session

`S` · **Pipeline** · _measurement; reopens the Parked AQ line only if it wins_

- **What.** Repeat [nvenc-aq-20261016](measurements/nvenc-aq-20261016/REPORT.md)
  where it was weakest:
  - on a 4K clip, since the size complaints are about 4K;
  - scored with VMAF v1 (P1.42's `--vmaf-v1`) beside v0.6.1 and CAMBI;
  - encoded through the player's direct NVENC session (`NvencDirect.cpp`), not
    FFmpeg's `hevc_nvenc` with the same settings.
- **Why.** The 1080p verdict was a 15-77 % BD-rate loss. That margin is wide,
  but the task asked for VMAF v1 and the direct session, and AQ does best on
  large flat areas at 4K.
- **Where.** `tools/benchmark/nvenc_aq.py`, `docs/measurements/`.

<a id="p260"></a>
### P2.60 · Re-time M1 on the RTX 4080 SUPER

`S` · **Pipeline** · _measurement_

- **What.** Run the timing halves of M1 on the RTX 4080 SUPER, as M1 planned:
  the 4K NV12 transport share (P2.57), the cost per neural pass (P1.41), and
  the model time on RenoDX 10.8.0 and 310.8.SF-v2, what 0.30.0 runs on an
  RTX 40 (P1.52 checks its picture). Every M1 and M1.2 run was on the RTX 5090.
- **Why.** At the 4080 SUPER's 4K pace (14.8 fps) the transport share should be
  smaller still. The cost per pass and its VRAM, about +505 MiB a pass on the
  5090, are what those users meet, and the pass ladder's descriptions quote them.
- **Where.** `tools/benchmark/m1.py`, `pipe_transport.py`, `docs/measurements/`.

<a id="p251"></a>
### P2.51 · Deinterlace interlaced sources before the model

`M` · **Pipeline** · _input conditioning; a cache- and export-key term_

- **What.** Detect interlaced sources (`field_order` and per-frame flags) and
  deinterlace them in the decode child (`bwdif`, frame rate kept) or with
  NVDEC's adaptive deinterlacer before the model sees the frame. On for sources
  flagged interlaced, with an off switch.
- **Why.** Nothing in `src/` deinterlaces a source (only interlaced display
  modes are filtered, `main.cpp`), so DVD rips and broadcast captures, which
  the player otherwise handles (the 0.27.0 anamorphic fix), reach the model
  combed. Asked for in [Veyra #41](https://github.com/Likely7/Veyra-NRVideo/issues/41)
  (2026-10-15). The Parked line grouped deinterlacing with tasks that need a
  model; this one does not.
- **Where.** `VideoDecoder.cpp`, `MediaPipeline.cpp`, `MediaTools.h`,
  `NeuralCache.cpp`, `ExportPipeline.h`.
- **Test.** 1080i and 480i fixtures show no combing in the rendered frame;
  progressive sources stay byte-identical.

<a id="p253"></a>
### P2.53 · Say a long job finished, outside the window

`S` · **Player**

- **What.** A Windows notification when an export, `--render` or whole-video
  render finishes or fails, with Open folder and Play result, and a level
  setting (All / Errors / None).
- **Why.** The in-app toast shows only inside the window; NeuralScreen v2.4.0
  added the level setting. Taskbar progress exists in `main.cpp`, but its
  comment records no fill on Windows 11 26200; check whether the shell's
  one-progress-per-button-group rule is the cause.
- **Where.** `main.cpp`, `ConvertMain.cpp`, `StatusNotePolicy.h`.

<a id="p264"></a>
### P2.64 · Measure, then reuse exact-duplicate source frames

`S` to measure, `M` to build · **Pipeline** · _ships only if byte-identical or better_

- **What.** When a decoded source frame is byte-identical to the one before
  it, reuse the previous rendered frame instead of running the model again.
  This happens with pulldown, frozen frames, slideshows and title cards.
- **Why.** NeuralScreen v2.6 skips still frames (681 of 681 frames processed
  became 0 of 866). Every skipped evaluation is a full frame of time saved.
- **Measure first.** Render a clip with known duplicates at Lossless and
  compare each duplicate's rendered frame with the frame before it. If they are
  byte-identical, reuse is quality-neutral and ships unconditionally. If they
  differ only by the model's temporal noise, reuse is arguably the more
  faithful picture, but that is a quality change. Then it is scored with P1.42
  and becomes a cache-key term, and ships only if it scores better.
- **Where.** `OfflineNeuralRenderer.cpp` (frame loop), the decode child's frame
  hashes, `NeuralReceipt.cpp` (count reused frames), `NeuralCache.cpp`.
- **Risk.** Temporal history: the model's next frame after a reused frame must
  see the same history it would have seen. Check the frame after each run of
  duplicates, not only the duplicates.

<a id="p268"></a>
### P2.68 · One cache entry for the previous runtime's two networks

`S` · **Pipeline** · _quality-neutral; a known inefficiency from 0.30.0_

- **What.** On the previous runtime (310.8.SF-v2), key network B the same as
  network A. Every preset value renders the same picture there, but the
  settings digest adds `network=a|b`, so B is cached apart from A's identical
  pixels and the same range renders twice
  ([runtime-310916-20261031](measurements/runtime-310916-20261031/REPORT.md)).
  The receipt still names the network the user chose.
- **Why.** A render is never made again for a choice that changes no pixel.
- **Where.** `NeuralCache.cpp`, the settings digest (`NeuralSettings.h`).
- **Test.** On the previous runtime, A then B on one range renders once. On
  310.9.16, A and B still key apart.

<a id="p265"></a>
### P2.65 · Say in the exported file how it was made

`S` · **Pipeline** · _metadata only; no pixel changes_

- **What.** Write the tool and version, the runtime digest, the neural preset
  and network (A or B, since 0.30.0), the processing scale, and an "AI-modified"
  marker into exported MP4 and MKV metadata and PNG text chunks. The values
  come from the receipt the player already keeps. Source metadata is still
  copied (`-map_metadata`); these tags are added beside it.
- **Why.** Every export is AI-modified video, and the public argument about
  DLSS 5 is whether it changes what was there. Today an export copies the
  source's metadata and adds nothing of its own (`MediaPipeline.cpp`), so a
  render can be passed off as the original. EU AI Act Article 50 has applied
  since 2026-08-02. Whether it covers a tool that only enhances is
  unresolved, so the docs claim compliance neither way.
- **Where.** `MediaPipeline.cpp`, `ExportPipeline.h`, `NvencDirectMux.h`,
  `CompareImageIO.h` (PNG), `ConvertMain.cpp`, `docs/USAGE.md`.
- **Test.** A tag round-trip test through FFprobe for MP4, MKV and PNG. A stream
  copy still keeps the source's title and chapters.

<a id="p238"></a>
### P2.38 · Measure DLSS SR presets L and M

`S` · **Pipeline** · _measurement first_

- **What.** Run presets L and M through the six-clip harness against bicubic.
  Add them as rungs only if they win; otherwise re-date the README's verdict.
- **Why.** `DLSSBackend.cpp:225-232` forces preset K in every mode. The
  2nd-generation transformer
  ([NVIDIA, 2026-01-06](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/))
  post-dates the "loses to bicubic" measurement, and the pinned 310.9.1 DLL
  defines L and M. Veyra 2.0.7 (2026-10-12) now offers K, M and L, and
  dlssg_for_sm86 0.4.0 runs L and M on RTX 20/30 in INT8.
- **Where.** `DLSSBackend.cpp`, `UpscalingPolicy.h`, `docs/measurements/`.

<a id="p239"></a>
### P2.39 · RTX VSR on the neural render

`M` · **Player, Pipeline** · _must beat bicubic on the harness first_

- **What.** RTX VSR as a display upscaler and export stage after the neural
  pass, not only on the original.
- **Since 0.28.1.** It beat bicubic on the harness (`vsr-quality-20261002`) and is
  the default playback upscaler - in cached playback that includes the render in
  the DLSS 5 view - and the default export upscaler for Super Resolution without
  the model (`VsrUpscalePass`). Left: VSR after the neural pass in an export
  (`sr`+`nr` still runs DLSS's carrier), and a measurement of VSR on a neural
  render, which has no ground truth to score against.
- **Why.** `VsrPolicy.h` runs VSR on the original only. DLSS5Tool, Visual
  Enhancer's Upscale tab and a fork do VSR after the neural pass;
  [NeuralScreen #125](https://github.com/perseval-BLR/NeuralScreen/issues/125)
  asks for it.
- **Where.** `VsrEngine.cpp`, `UpscalingPolicy.h`, `ExportPipeline.h`,
  `OfflineNeuralRenderer.cpp`. Becomes an export key term.
- **Candidate.** The NVIDIA Video Effects SDK's VSR "HighBitrate" mode, meant
  for clean sources, may suit a neural render better than RTX VSR (P3.15).

<a id="p240"></a>
### P2.40 · Opt-in subtitle burn-in for MP4 and GIF

`M` · **Pipeline** · _lower value; MKV already carries subtitles_

- **What.** Reuse the playback subtitle compositing in the export path, off by
  default.
- **Why.** Listed as unsupported in the README; soft subtitles do not survive
  MP4-only platforms or re-uploads. Nothing is burned in today
  (`MediaPipeline.cpp:1014`).
- **Where.** `ExportPipeline.h`, `SubtitleOverlay.cpp`, `AppMenu.cpp`.

## Interface, input and convenience

<a id="p267"></a>
### P2.67 · A native speaker's review of the Simplified Chinese text; more languages

`S` · **Player, Docs** · _takes contributions; answers P2.63's owner decision_

- **What.** 0.30.0 ships `lang/zh-Hans.lang`, drafted by an AI model and not
  yet reviewed by a native speaker. Ask for that review in an issue, take
  fixes and new languages as pull requests (`--lang-template` writes the file
  to start from), and say in `CONTRIBUTING.md` how. Until a review lands, the
  docs call the file unreviewed.
- **Why.** P2.63 left one owner decision: who reviews text in a language the
  maintainer may not read. The answer is a native speaker, through a pull
  request.
- **Where.** `lang/zh-Hans.lang`, `TranslationFile.h`, `CONTRIBUTING.md`,
  README.
- **Test.** No contributed file lacks a key or carries one English lacks, and
  every format placeholder survives. A layout smoke at the file's longest
  strings.

## HDR

<a id="p256"></a>
### P2.56 · Try an FP16 scRGB swap chain for HDR presentation

`S-M` · **Player** · _presentation only; verify before switching_

- **What.** Present HDR originals through an FP16 scRGB swap chain instead of
  R10G10B10A2 HDR10, after measuring both on the wire.
- **Why.** One capture study on Blackwell found FP16 scRGB exact to rounding,
  while R10G10B10A2 showed 49 two-code jumps and 600 nits arriving as 603.8
  ([HDR swap-chain capture](https://github.com/yadonpapa/20260830_HDR-Swapchain-Capture)).
  One source and one GPU generation, so measure first.
- **Where.** `D3D12Renderer.cpp`, `HdrPolicy.h`.

## Tests

<a id="p269"></a>
### P2.69 · A YouTube resolver test that fails under load

`S` · **Release** · _a flaky test; no product change_

- **What.** `youtube_resolver_repeated_runs_leave_process_handle_count_stable_test`
  failed 1 in 4 direct runs under load during 0.30.0. It gives each resolve
  500 ms. Wait for each resolve to end, with a bound that holds on a busy
  machine, and keep the handle-count check.
- **Why.** A test that fails at random teaches people to re-run failures.
- **Where.** `tests/PolicyTests.cpp`.
- **Test.** Twenty direct runs pass while another build loads the machine.

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

P3.14 landed in 0.30.0 (2026-10-31), milestone M1.2, with P3.5's RTX 50 half:
DLSS-NR 310.9.16 on RTX 50 with networks A and B, RenoDX 10.8.0 everywhere,
and the previous runtime one choice away
([runtime-310916-20261031](measurements/runtime-310916-20261031/REPORT.md)).
What it left open is P1.52, P2.68 and the rest of P3.5.

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
- **Reference.** NVIDIA's own design, in RTX Remix (MIT): an ACES fit to LDR,
  the model in LDR, then a per-pixel ratio back to the original HDR with a
  highlight-recovery blend
  ([inverse tone map](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/src/dxvk/shaders/rtx/pass/tonemap/tonemapping_inverse_tonemapping.comp.slang),
  [ecf646d](https://github.com/NVIDIAGameWorks/dxvk-remix/commit/ecf646d)).
  It is the per-pixel ratio design above, plus highlight recovery.
- **Risk.** Halos where the model relit the scene.

<a id="p318"></a>
### P3.18 · A measured HDR→SDR peak for the model's input

`M` · **Pipeline** · _opt-in input conditioning; cache-key term_

The player maps HDR from static MaxCLL and mastering metadata (`HdrPolicy.h`,
`HdrToneMap.h`), which is MPC Video Renderer's "Metadata" method. MPC VR's new
"Measured" method takes each frame's peak from a compute histogram, smooths it
over 3 s with a 640-nit floor and resets at cuts
([PR #292](https://github.com/Aleksoid1978/VideoRenderer/commit/1c0715065dcdd5c64afb2b023f48532e35ea683d),
2026-10-14); libplacebo's master moved to per-cone tone mapping on maxRGB scene
statistics (LGPL; reimplement from the description). Both handle wrong or
missing metadata. Measure for brightness pumping before offering it.

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
have one; the README lists it as unsupported in the player, and watch folders
are parked on the grounds that `dlss5-convert` covers batch work.

<a id="p315"></a>
### P3.15 · NVIDIA Video Effects SDK: frame generation and VSR modes

`M-L` · **Pipeline** · _Ada and Blackwell only_

**Blocker:** the SDK's licence, and whether its NGC-hosted models may be
redistributed.

Video Frame Generation (2x or 4x, an exact target rate by timestep, scene-cut
bypass, 10-bit) as a frame-generation export engine beside DLSSG plus NVOFA,
and VSR's HighBitrate and Deblur modes as candidates for upscaling the neural
render (P2.39)
([docs](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html)).
Visual Enhancer v16.0, Veyra 2.0.6-2.0.7 and Lunarc 1.2.2 adopted them within
two weeks. Measure against DLSSG on the frame-generation harness first, and
offer it as a labelled engine choice, never a silent switch.

<a id="p316"></a>
### P3.16 · Frame Generation "Performance mode" as a labelled rung

`S` · **Pipeline** · _watch; never the default_

**Blocker:** it is a driver and NVIDIA app override (Preset C, app 11.0.9.565
beta), and Streamline 2.14.1's `sl_dlss_g.h` has no preset field. Veyra 2.0.8
reaches it through game-harvested 310.9.16 DLLs, which the player does not
take. Offer it only once a public SDK exposes it, as a rung with its measured
cost.

<a id="p317"></a>
### P3.17 · A JavaScript runtime for yt-dlp after Deno

`S` · **Release** · _watch_

Deno's development ends after a year of maintenance releases
([deno.com, 2026-10-09](https://deno.com/blog/cloudflare)), and yt-dlp is
choosing a new default runtime
([#17799](https://github.com/yt-dlp/yt-dlp/issues/17799)). The player bundles
Deno 2.9.5 for `--js-runtimes`. Follow #17799 and swap the bundled runtime when
yt-dlp settles, hash-pinned like the rest.

Since 2026-10-31: there is still no decision. A yt-dlp contributor says Deno
stays recommended "until new announcements", and that logged-out extraction
currently needs no runtime at all
([#17904](https://github.com/yt-dlp/yt-dlp/issues/17904)). Deno 2.9.8 shipped
on 10-21. 0.30.0 took the new yt-dlp (P0.20) but still bundles Deno 2.9.5.

<a id="p319"></a>
### P3.19 · Overlap the per-frame stages

`L` · **Pipeline** · _quality-neutral; a real restructuring_

From the 2026-10-01 survey. Run guides for frame N+1 while NR runs on N and
capture and encode on N-1, with per-stage fences replacing the capture fence
wait inside the 7.35 ms fixed term (`docs/ARCHITECTURE.md`). The one-frame
`FramePrefetch` is the only overlap the current loop allows. It must not redo
anything on the "already optimised" list.

Since 2026-10-31: MPCVR-Plus 2.5 made NR and its renderer wait on each other on
the GPU instead of on the CPU, and went from 26.2 to 19.6 ms a frame
([MPCVR-Plus](https://github.com/HumbleUser33/MPCVR-Plus/releases), author's
numbers). It is the same kind of change, and evidence that it pays. Re-fit the
fixed term on 310.9.16 (in 0.30.0) first, since a faster model makes the
fixed overhead a larger share of each frame.

<a id="p320"></a>
### P3.20 · A residual-composite reduced-scale rung

`M` · **Pipeline** · _opt-in labelled rung_

From the 2026-10-01 survey. Run the model at 75 % or 50 % and composite its
upscaled change onto the native frame, instead of upscaling the result with
DLSS SR, so native-resolution text and edges survive (NeuralScreen's Boost).
It mainly fixes the softness of today's 75 % and 50 % rungs rather than adding
speed. Its own cache-key term; never a default.

Since 2026-10-31, three tools ship this design: MPCVR-Plus 2.1+ (NR working
size 50-100 %, the change applied back at full size), Veyra 2.1 (25-100 %) and
DLSS5Tool v3.1 (GPU residual compositing). Its design is no longer in doubt;
whether it is worth building depends on its measured quality on the P1.42
yardstick.

<a id="p321"></a>
### P3.21 · Opt-in C2PA content credentials on exports

`M` · **Pipeline, Release** · _after P2.65; shares a certificate decision with Authenticode_

Write a [C2PA](https://c2pa.org/) manifest on exported MP4, PNG and GIF. It
carries `c2pa.edited` with a `trainedAlgorithmicMedia` source type and the
receipt's runtime and settings, chained to the source's own manifest when the
source has one. [c2pa-rs](https://github.com/contentauth/c2pa-rs) (0.91.7,
2026-10-26) has a C library and supports MP4, MOV, GIF and PNG, but not MKV,
so an MKV export gets P2.65's tags only. YouTube reportedly labels
C2PA-declared AI content (unverified), and platforms are reported to strip
manifests from video (unverified). A self-signed manifest shows as
"untrusted", so a trusted one needs the same paid certificate as Authenticode.
Azure Artifact Signing takes individuals only in the US and Canada, and
SignPath Foundation excludes projects that bundle proprietary components,
which the NVIDIA DLLs are. Opt-in; never claim it makes an export compliant
with anything.

---

<a id="p35"></a>
### P3.5 · NVIDIA's signed DLSS-NR runtime, and 310.9.16 beyond RTX 50

`M` · **Pipeline, Release** · _the RTX 50 half shipped in 0.30.0, unsigned_

**Blocker:** NVIDIA's signed DLSS-NR 310.9.16 is not available from the
approved sources, and the licence under which it may be redistributed is
unknown.

**What 0.30.0 found**
([runtime-310916-20261031](measurements/runtime-310916-20261031/REPORT.md)):

- The mirror's `nvngx_dlssnr.dll` 310.9.16 is not NVIDIA's signed build. It is
  modified and NotSigned, with an added `.nrcubin` section (sm_75, sm_86 and
  sm_89 kernels). RTX 50 ships that build. The "modified, unsigned" notice is
  unchanged.
- NVIDIA's official DLL fails feature creation on an RTX 3060 Ti with
  `0xBAD00002`
  ([NeuralScreen v2.6.1](https://github.com/perseval-BLR/NeuralScreen/releases/tag/v2.6.1)).
  RTX 20-40 keep 310.8.SF-v2 (P1.52).

**What stays open**

- **The signed build.** Find a signed `nvngx_dlssnr.dll` 310.9.16 from a
  source that can be approved, and its licence. Candidates: the public RTX
  Remix 1.6.1 zip, which should carry one from the packman package
  `ngx_sdk_dlnr` v2 (dxvk-remix 09808cc; its version is unread), and the
  driver's `%ProgramData%\NVIDIA\NGX\models` copy (driver 616.64 and later),
  which might be used instead of shipping one. Check its Authenticode
  signature, as NeuralScreen v2.6 does, and prefer it on RTX 50 only if it
  renders the same networks. The RenoDX add-on that drives it stays unsigned,
  so the notice narrows but does not go away.
- **Other generations.** Whether the mirror's 310.9.16 renders on RTX 20, 30
  or 40, as its sm_75/86/89 kernels suggest, and with what picture. It goes
  through the same gate as 0.30.0's RTX 50 run before any generation moves off
  310.8.SF-v2.
- **Test.** The runtime smokes and `tools/benchmark/m12.py` per generation;
  the preflight and receipt name the file, its version and its signature
  state, as they do since 0.30.0.

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
| **Watch folders** | Requested at Topaz since 2022 and never shipped, with no sign that users urgently need it. `dlss5-convert` covers the batch case: files, folders and wildcards, skipping what is done. |
| **Stabilization, colorization, face restoration** | Each needs a model we cannot obtain. Inferior versions dilute a single-model product. (Deinterlacing was listed here until 2026-10-16; it needs no model and is P2.51.) |
| **Model reuse every Nth frame; residual amplification above 1×** | Both trade stability or fidelity for speed or punch ([optiscaler-dlss5](https://github.com/Skynizz/optiscaler-dlss5), Veyra 2.0.4), and the runtime renders Intensity above 1.00 as 1.00 (0.28.2). |
| **Live window or desktop capture** | A different product. Visual Enhancer removed its Live workspace in v16.0 (2026-10-14). |
| **8K/16K output, 480 fps frame generation** | Marketing checkboxes that no panel can show. Cadence-aware multiple selection is the right design, so say that instead. |
| **Cloud rendering / credits / tiering** | Being MIT and free is an asset against a closed competitor. |
| **Linux or mobile ports, multi-GPU splitting of one render, "auto" model recommendation** | Feature 18 on Windows D3D12 is the product. Separate GPU choices for AI and for NVENC is the useful form (`GpuPreference.cpp`). |
| **Running the model on AMD or Intel** | Community projects do it (DLSS-NR-on-AMD, an Xe2 port); the product is RTX-only by design. |
| **Full zero-copy decode/encode rewrite** | The ffmpeg child is load-bearing: codec coverage, process isolation for the runtime lock, and the benchmark harness. Only the narrow encode side was worth doing, and it shipped as direct NVENC. The NV12 pipe left costs about 2 ms of a 19.2 ms 4K frame, on the decoder thread and off the critical path ([nv12-transport-20261016](measurements/nv12-transport-20261016/REPORT.md)). |
| **NVENC adaptive quantisation and lookahead** | They lose at matched size: +15 % to +77 % more bits for the same VMAF, and lower VMAF at the shipped size; the only gain is a little less banding on the 8-bit rung ([nvenc-aq-20261016](measurements/nvenc-aq-20261016/REPORT.md)). Scored at 1080p with v0.6.1 through FFmpeg's encoder; P2.59 checks 4K with VMAF v1 before this is final. |
| **A preset from NVIDIA's RTX Remix NR defaults** | It lands at 67 % of Standard's distance from the source, against Gentle's 64 %, so on strength it is a second Gentle ([nvidia-defaults-20261016](measurements/nvidia-defaults-20261016/REPORT.md)). Only as a named "NVIDIA reference" look, if the owner wants one. |
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

**2026-10-16, against f42cb39 (0.28.2).** No code audit. Three research agents,
read-only: the DLSS 5 ecosystem through `gh` (releases, commits and issues
since 2026-10-01, new repositories, impostors); NVIDIA, Microsoft, FFmpeg,
yt-dlp and metrics at primary sources; adjacent players, review tools and
papers. The lead re-checked the claims that create tasks: the yt-dlp breakage
and fix, the dxvk-remix commit and its files, the rhi-repo releases, the
Visual Enhancer v15.0 and NeuralScreen v2.2.0 notes, Veyra #41, the RenoDX
repository's add-ons, and the matching absences in `src/` (power requests,
control mask, deinterlacing, back-step, resume position, frame-time term).
Findings and links: `reports/landscape-refresh-2026-10-16.md`.

**2026-10-31, against 8dfded2 (0.28.3).** No code audit. Three research agents,
read-only, covering 2026-10-16 to 10-31:

- the DLSS 5 ecosystem through `gh`: the runtime mirror, dxvk-remix,
  competitors' releases and issues, impostors, `github/dmca`;
- NVIDIA, Microsoft, FFmpeg, yt-dlp, VMAF, C2PA, the EU AI Act and code
  signing at primary sources;
- the wider field: players, enhancement apps, review tools, papers and
  distribution conventions.

The lead re-checked the claims that create or move tasks:

- the rhi-repo release list;
- dxvk-remix 09808cc's NR diff (preset B, control mask, tone strength);
- certifi's advisory and yt-dlp 2026.10.21's notes;
- Visual Enhancer v20.0's notes;
- the matching facts in `src/`: `NRPreset` defaulting to 0, `Localization.h`
  having only English, `SceneCut.h`, exports adding no metadata of their own,
  no Agility SDK, and `UpscalingGpuSmoke`'s `RemoveDevice` case.

Findings and links: `reports/landscape-refresh-2026-10-31.md`.
