# Improvement task list

_Audited against 0.25.0 (23d71d9) on 2026-09-23._

Open work only, highest priority first. Completed items are removed rather
than ticked; what shipped is in `CHANGELOG.md` and git history. When a task
lands, delete it from this file in the same change.

Sources: three code audits (playback/audio/UI, neural helper/cache/runtime,
build/CI/release/docs), two web surveys (pipeline and competitive landscape;
player UX and comparison tooling), and the open items carried over from the
2026-09-20 list after re-checking them against current source.

## How to read it

| Mark | Meaning |
| --- | --- |
| **P0** | A bug users hit, or a published promise that is broken. Fix before the next release. |
| **P1** | Reliability, quality-neutral performance, CI and docs. The next few weeks. |
| **P2** | High-value features, from the research. Each is justified on its own. |
| **P3** | Large or strategic, or waiting on something outside this repo. |
| ✅ | Re-read against source at 23d71d9 while this list was written |
| 🔍 | Audit finding cited to file:line, not independently re-read |

**Impact** names what a task changes: **Player** (what the viewer sees and
feels), **Pipeline** (render, cache, export, helper), **Release** (packaging,
CI, supply chain), **Site** or **Docs**. File references are relative to
`src/` unless a path is given.

**Baseline at this audit.** Release build clean with **0 warnings at /W4**.
CTest: **26 tests, 24 pass, 2 expected skips** on an RTX 4080 SUPER
(`DlssgEvaluateSmoke` needs Blackwell; `FrameGenerationSmoke`'s clip is not
fetchable), 175 s. `site/test.ps1`: 31 pass.

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
| **P0** | | | | |
| [P0.1](#p01) | The packaged `verify_package.ps1` cannot run | S | Release | ✅ |
| **P1** | | | | |
| [P1.10](#p110) | The first-frame receipt gate: cost and reproducibility | S | Pipeline | 🔍 |
| [P1.14](#p114) | Live-session write amplification | M | Pipeline | 🔍 |
| [P1.15](#p115) | Small render-thread costs | XS each | Pipeline | 🔍 |
| [P1.16](#p116) | Duplicated helpers that behave differently | M | Pipeline | 🔍 |
| [P1.17](#p117) | The GPU CI workflow cannot pass on a fresh runner | S | Release | 🔍 |
| [P1.18](#p118) | The attestation workflow signs a digest someone typed | S | Release | ✅ |
| [P1.19](#p119) | No PDBs for crash dumps; builds are not reproducible | S | Release, Player | 🔍 |
| [P1.20](#p120) | CI hardening | M | Release | 🔍 |
| [P1.21](#p121) | Build structure: one set of objects, one set of flags | M | Release | 🔍 |
| [P1.22](#p122) | Test hygiene and coverage gaps | M | Release | 🔍 |
| [P1.23](#p123) | Docs, screenshots and positioning drift | S | Docs, Site | ✅ |
| **P2** | | | | |
| [P2.1](#p21) | Supply a smoothed exposure instead of auto-exposure | S | Pipeline | ✅ |
| [P2.2](#p22) | Dither wherever the image is cut to 8 bits | S | Pipeline, Player | ✅ |
| [P2.3](#p23) | A quality ladder for cache and export: CQ, 10-bit, lossless | M | Pipeline | ✅ |
| [P2.4](#p24) | Guide A/B harness, then evaluate Video Depth Anything | S / M-L | Pipeline | |
| [P2.5](#p25) | Temporal stability with motion compensation | M | Pipeline, Player | |
| [P2.6](#p26) | Spatial mask and feather; face protection later | S-M | Pipeline, Player | |
| [P2.7](#p27) | Processing scale | S-M | Pipeline | |
| [P2.8](#p28) | RTX Video Super Resolution as a second engine | M | Pipeline, Player | |
| [P2.9](#p29) | Super Resolution-only export | M | Pipeline | |
| [P2.10](#p210) | CLI / headless invocation | S | Pipeline | |
| [P2.11](#p211) | Quality metrics in the app | M | Player, Pipeline | |
| [P2.12](#p212) | Scene-cut controls and duplicate-frame handling | S / M | Pipeline | |
| [P2.13](#p213) | Re-measure which settings change the image on RenoDX 6.5.3 | S | Pipeline | |
| [P2.14](#p214) | A deband pre-pass for compressed sources | S-M | Pipeline | |
| [P2.15](#p215) | An on-screen compare bar with press-and-hold A/B | S | Player | |
| [P2.16](#p216) | Zoom, pan and a synced magnifier | M | Player | |
| [P2.17](#p217) | A difference view | S-M | Player | |
| [P2.18](#p218) | The timeline as a render map | S / M | Player | |
| [P2.19](#p219) | Status chips instead of one overflowing line | S | Player | |
| [P2.20](#p220) | A start screen with a capability check and tiles | M | Player | |
| [P2.21](#p221) | Export the comparison itself | S / M | Player | |
| [P2.22](#p222) | Dark, DPI-aware menus and dialogs | M | Player | |
| [P2.23](#p223) | Keyboard discoverability | S-M | Player | |
| [P2.24](#p224) | Media controls and taskbar buttons | S-M | Player | |
| [P2.25](#p225) | Synced multi-pane comparison | M-L | Player | |
| [P2.26](#p226) | A comparison gallery on the site, plus site fixes | M | Site | |
| **P3** | | | | |
| [P3.1](#p31) | HDR end to end | L | Pipeline, Player | |
| [P3.2](#p32) | Subtitles via libass, composited after the network | M-L | Player | |
| [P3.3](#p33) | WASAPI drift correction and passthrough | M | Player | |
| [P3.4](#p34) | Extract testable units from `main.cpp` | M | Player | |
| [P3.5](#p35) | Prefer NVIDIA's signed runtime on RTX 50 | M | Pipeline, Release | |
| [P3.6](#p36) | Neural optical flow as an export-only rung | M-L | Pipeline | |
| [P3.7](#p37) | Feed NVENC directly from D3D12 | M | Pipeline | |
| [P3.8](#p38) | Repository media hygiene | S | Release | |

---
---

# P0 — Fix now

<a id="p01"></a>
### P0.1 · The packaged `verify_package.ps1` cannot run

`S` · **Release** · ✅

**Where** — `tools/verify_package.ps1:13-14`, `:166`, `:199`, `:204`, `:213`;
`README.md:52-56`

The script ships inside both zips, and the README tells users to run
`.\verify_package.ps1 -StageDirectory .` from the unpacked folder. On its
first line it sets `$repositoryRoot` to its parent directory and reads
`VERSION` from there. In a package that file does not exist, so the script
fails before checking anything. It also reads `packaging/*.json` and
`external/DLSS/...`, which are not packaged either. The README command also
omits `-PublicCore` for the core zip.

**Impact** — Release: the check the README offers users does not work, so
nobody can verify what they downloaded.

**Fix** — add a package mode that takes the version from `PACKAGE_MANIFEST.txt`
or the folder name, and checks against the manifest's hashes. Have CI run it
from an extracted zip with no repository around it. Correct the README command.

---

---

# P1 — Next

## Player: reliability and quality-neutral performance

## Pipeline: helper, cache, runtime

<a id="p110"></a>
### P1.10 · The first-frame receipt gate: cost and reproducibility

`S` · **Pipeline** · 🔍 · _includes the log-polling row of old 2.10_

**Where** — `OfflineNeuralRenderer.cpp:1362-1406`, `:1886-1893`, `:2221-2244`

Each resubmit of frame 0 is a synchronous full capture and readback. Each
log read waits at least 200 ms for the file to stop growing. A cold job
resubmits about 60 times with history building up, and the number depends
on when the log flushes, so **the same cache key can produce different
bytes on different runs**.

**Fix** — resubmit without capturing, capture once when the gate opens, and
reset history immediately before that capture. Tail the log from the last
offset instead of re-reading it.

---

<a id="p114"></a>
### P1.14 · Live-session write amplification

`M` · **Pipeline** · 🔍 · _what is left of old 2.9_

A live session writes every rendered byte twice and reads it three times.
The segments are joined into a full `staging/neural.mkv` copy, and the
segments stay until the session is released, because `SynchronizedPlayback`
keeps reading them after the join. Peak disk is about twice the render.

**Fix** — once the joined entry is published, switch playback onto the
published payload (a `SynchronizedPlayback` retarget) and delete the
segments, or publish by concatenating straight into the entry's staging
directory with no intermediate copy.

---

<a id="p115"></a>
### P1.15 · Small render-thread costs

`XS each` · **Pipeline** · 🔍 · _open rows of old 2.10_

| Item | Where | Fix |
| --- | --- | --- |
| The capture fence wait runs on the render thread | `OfflineNeuralRenderer.cpp:1984` → `D3D12Renderer.cpp:1290` | Move `BeginResolveOldestCapture` into the `DeferredCapture` worker |
| The recycle pool holds 4 buffers against a queue of about 30 frames | `:446` vs `:320` | Size the pool to the queue depth (a miss is a 33 MB memset at 4K) |
| Telemetry vectors have no `reserve` | `:118-135` | Reserve for the frame count |
| `SelectSegment` returns a segment by value under the index mutex | `SynchronizedPlayback.cpp:429` | Return an index |
| `Log::Write` holds a global mutex across `OutputDebugStringA` and a flushed write | `Log.h:14-18` | Skip `OutputDebugStringA` unless a debugger is attached; flush on a timer |

---

<a id="p116"></a>
### P1.16 · Duplicated helpers that behave differently

`M` · **Pipeline** · 🔍 · _open parts of old 2.21_

These matter because the copies **disagree**, not because they are repeated.

- **Wide/narrow conversion**: the parent and child of the same IPC channel
  handle non-ASCII differently. `NeuralWorker.cpp:109` rejects it;
  `NeuralWorkerMain.cpp:149` substitutes `?`.
- **`JsonEscape`**: `NeuralCache.cpp:120-139` returns an empty string for a
  control character, while `NeuralPreflight.cpp:469-492` escapes it.
- **Hex and NGX error formatting**: 24 ad-hoc `std::hex` sites drop leading
  zeros, so their log lines cannot be grepped against receipts.
- **`CreateKillOnCloseJob`**: identical copies at `MediaPipeline.cpp:114` and
  `NeuralWorker.cpp:79`.
- **Atomic file writes**: five copies, and only two of them flush.
- **`AudioPlayer.cpp:34`'s ffmpeg lookup** lacks the `neural-runtime` guard the
  other two lookups have, and falls back to `SearchPathW`, so it can pick up
  an arbitrary ffmpeg from PATH.

**Fix** — one shared header per helper, following `Sha256File`'s single
implementation.

## Release and CI

<a id="p117"></a>
### P1.17 · The GPU CI workflow cannot pass on a fresh runner

`S` + owner decision · **Release** · 🔍 · _includes the open runner item of old 2.14_

**Where** — `.github/workflows/gpu-tests.yml`,
`tools/fetch_neural_runtime.ps1:160`

- It never stages the runtime and the ReShade inis into
  `build-upscaling/Release/neural-runtime`. `fetch_neural_runtime.ps1` only
  validates, and checkout's `git clean -ffdx` wipes that folder, so the
  neural smokes fail.
- The "refuse skips" step runs the whole hardware suite a second time and
  ignores that run's exit code. Its threshold says 10 tests; there are 13.
- There is still no self-hosted runner. Registering one needs the owner's
  credentials, so that part is the owner's decision.

**Fix** — add `stage_runtime.ps1 -Destination build-upscaling/Release/neural-runtime`
and copy the ini files. Parse `--output-junit` from the first run instead of
running the suite again.

---

<a id="p118"></a>
### P1.18 · The attestation workflow signs a digest someone typed

`S` · **Release** · ✅

**Where** — `.github/workflows/attest-release-asset.yml:50-69`

The workflow attests whatever digest the operator enters, and never downloads
the published asset to confirm it. Its inputs are also interpolated straight
into bash (`digest='${{ inputs.asset-digest }}'`). Only people with write
access can trigger it, but that is the classic script-injection shape.

**Fix** — `gh release download` the asset, compute its SHA-256 inside the
job, and attest that. Pass inputs through `env:`.

---

<a id="p119"></a>
### P1.19 · No PDBs for crash dumps; builds are not reproducible

`S` · **Release, Player** · 🔍

`CrashDump.h` writes minidumps from both processes, but Release links without
`/DEBUG`, so the dumps cannot be symbolised. There is no `/Brepro`, and zip
entries keep file mtimes (`tools/package_release.ps1:329-334`), so nobody can
rebuild the core zip to check it against the attestation.

**Fix** — build both shipped targets with `/Zi` and link with
`/DEBUG /OPT:REF /OPT:ICF`. Adding `/DEBUG` turns off the linker's default
REF/ICF, so state those explicitly. Publish the PDBs as a CI artifact, not
in the zip. Add `/Brepro` and fixed zip timestamps.

---

<a id="p120"></a>
### P1.20 · CI hardening

`M` · **Release** · 🔍 · _old 2.17, plus new items_

- [ ] **Split permissions.** `release.yml` gives `contents: write` and
      `id-token: write` to the job that builds and runs the fetch scripts, and
      `pages.yml` grants `pages`/`id-token` to the whole workflow. Separate a
      read-only build job from a small publish job.
- [ ] Set `timeout-minutes` on build, release and pages; today only
      gpu-tests has one, and the rest get the 6-hour default.
- [ ] Add `.github/dependabot.yml` for `github-actions`, so the SHA pins can
      be refreshed.
- [ ] **Turn on `/WX`.** The /W4 count is now 0, so this is free.
- [ ] Run `/analyze` on `DLSSVideoPlayer` and `NeuralWorker`, and
      `/fsanitize=address` on the device-free suites, in a separate quality job.
- [ ] Cache the pinned downloads (about 300 MB per run), keyed on
      `tool-lock.json`.
- [ ] Add a `pull_request` trigger to `pages.yml`, so the 31 site tests run
      on PRs.
- [ ] Set `persist-credentials: false` on `actions/checkout`.
- [ ] Add a `CMakePresets.json` and pin the MSVC toolset version.

---

<a id="p121"></a>
### P1.21 · Build structure: one set of objects, one set of flags

`M` · **Release** · 🔍 · _old 2.11 and 2.12_

There is no `add_library` anywhere, so 175 compiles build about 53 unique
files: `VideoDecoder` and `MediaSource` 12× each, `NeuralCache` 10×. Worse,
**the copies are built with different flags**. Six test targets lack
`/Zc:__cplusplus`, and `UpscalingGpuSmoke` lacks the generated include
directory (`CMakeLists.txt:294`, `:314`). So the tests are not exercising the
object code that ships.

**Fix** — three `OBJECT` libraries (`player_media`, `player_render`,
`player_neural`) with one shared set of options. Then enable
`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE` behind `check_ipo_supported` and
measure the change. Leave `/arch:AVX2` and `/fp:fast` alone: the first drops
pre-Haswell CPUs, and the second breaks the bit-identical renders the
benchmark relies on. Update the stale counts in the comments at
`CMakeLists.txt:46`.

---

<a id="p122"></a>
### P1.22 · Test hygiene and coverage gaps

`M` · **Release** · 🔍

- **Wall-clock ceilings that can flake on a loaded runner**:
  `PolicyTests.cpp:6020` (under 2,000 ms), `NeuralWorkerTests.cpp:786`,
  `:939`.
- **Sleep-then-stop at `NeuralPrerenderTests.cpp:1308`, `:2538`** can end up
  testing cancel-before-block instead of the blocked path. Use a latch.
- `assertion_count` (`tests/TestSupport.h:23`) is counted but never enforced,
  and `harness_sanity_test` (`PolicyTests.cpp`) counts `CHECK(true)` as a
  test.
- **Never run by CI**: `DLSSGBackend`, `FrameGenerationPass`, `WasapiRenderer`,
  `NeuralPreflightProbe` and `NeuralWorkerMain` run only under the `gpu`/`audio`
  labels (see P1.17). `CrashDump.h` and `PrecisionSleeper.h` have no tests.
  `stage_runtime.ps1` and the full-package path of `verify_package.ps1` never
  run (see P0.1). The Debug configuration is never built.
- **`FrameGenerationSmoke`'s clip cannot be obtained**, so the test skips
  permanently. Generate the clip in the test, or fetch it with a pinned hash.
- `tools/fetch_youtube_helpers.ps1` has four `# TEST-SEAM:` markers and no
  harness uses them. Add a Pester test for the restore-failure path.

## Docs and site

<a id="p123"></a>
### P1.23 · Docs, screenshots and positioning drift

`S` · **Docs, Site** · ✅ rows 1 and 4 · 🔍 the rest

| Item | Where | Fix |
| --- | --- | --- |
| Says `NvencPreset` defaults to 7; the player's default is 5 (`NeuralCache.h:139`). The helper's fallback of 7 is a deliberate wire contract | `docs/USAGE.md:281` | Say 5 |
| Screenshots show the old menus ("Upcoming games", File > Export cached video) and a v0.21.0 capture, while the README says they show "screens that haven't changed" | `docs/screenshots/current/recent-videos.jpg`, `neural-strength.jpg`; site "How it works" | Re-shoot |
| The site's `<title>`, `og:title` and `twitter:title` lead with "AI video upscaling", which is commoditised and not what the product does by default | `site/src/index.html:16`, `:24` | Lead with neural rendering and the same-frame comparison |
| The window title and error boxes read "DLSS Video Player" | `Localization.h:20` (`app.title`), `main.cpp:948`, `:7818` | "DLSS 5 Video Player" |
| The related-projects page is three weeks stale and says so. It also ships in both packages while pointing at this file, which is not packaged | `docs/RELATED_PROJECTS.md` | Rewrite it against the landscape below. Narrow the claim to: *the only one that renders the whole video progressively, keeps every frame, and shows the original and the render on the same frame while it is still rendering*. Lead with verifiability. Stop implying frame generation is part of live playback |
| The changelog is 189 KB (0.24.0 alone is 50 KB) and ships in the zips | `CHANGELOG.md` | Keep user-visible bullets; move the rationale into commits or `ARCHITECTURE.md` |
| Stale counts: "ten tests" (there are 13), "twenty executables" | `gpu-tests.yml:5`, `CMakeLists.txt:46` | Update them |

**Landscape since 2026-09-01, for the rewrite.**
- **NVIDIA:** DLSS 5 shipped officially on 2026-09-03 (driver 616.64, RTX 50
  only, one game), with RTX 40 support promised "later this fall" and no
  public SDK yet.
- **Merserk Visual Enhancer:** v2.1 → v11. It now has frame generation,
  10-bit HDR, RTX VSR, a Live mode, masks, shimmer suppression, a 2-Up view,
  and ProRes and FFV1 export.
- **NeuralScreen:** about 944★.
- **video2dlssnr:** GPU-resident pipeline.
- **dlss5-nr-player and its forks:** split and wipe views plus VSR.
- **A fork of this project:** ctype-lab.
- **ComfyUI packs:** new ones, including one with an automatic skin mask.
- **OptiScaler-DLSSNR:** issue #100 shows it failing on an offscreen DLAA
  harness like this one. RenoDX is still the only runtime known to work here.

---
---

# P2 — High-value features

Each of these is quality-first. Anything that changes pixels becomes a
cache-key term, and any trade-off ships as a ladder with a labelled default.

## Pipeline quality

<a id="p21"></a>
### P2.1 · Supply a smoothed exposure instead of auto-exposure

`S` · **Pipeline** · ✅ current state

`DLSSBackend.cpp:254` sets `AutoExposure`, and `:377` passes a null exposure
texture. NVIDIA's guide says to supply exposure whenever it is known. An
OptiScaler-DLSSNR PR found that the neural renderer's white point drifts under
lighting changes without it. On video, auto-exposure is a likely cause of
brightness pumping and of slow recovery after a cut.

**Do** — a GPU luminance meter, temporally smoothed (libplacebo's
`peak_smoothing_period` is the model), reset on `ClassifySceneCut`, written to
a 1×1 exposure texture. **Measure first**: `docs/BENCHMARK.md` already lists
this A/B as undecided. Ship it only if the benchmark's added-sigma and
flicker numbers improve.

Refs: [DLSS guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md) ·
[OptiScaler PR #77](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass/pull/77) ·
[libplacebo options](https://libplacebo.org/options/)

**Impact** — Pipeline: steadier tone across cuts and lighting changes.
Player: less visible pumping.

---

<a id="p22"></a>
### P2.2 · Dither wherever the image is cut to 8 bits

`S` · **Pipeline, Player** · ✅ current state

The only dithering in `src/` is the GIF palette (`MediaPipeline.cpp:693`). The
neural output goes from FP16 straight to an 8-bit capture, and the image
adjustments go straight to an `R8G8B8A8` swapchain. Add blue-noise dithering
(libplacebo's default, a 64×64 LUT) at both points.

**Impact** — Pipeline: less banding in the dark gradients the model lifts;
the capture-side dither is a cache-key term. Player: the same on screen,
where it costs nothing to the cache.

---

<a id="p23"></a>
### P2.3 · A quality ladder for cache and export: CQ, 10-bit, lossless

`M` · **Pipeline** · ✅ current state

The cache is always HEVC 8-bit at `-cq 16` (`MediaPipeline.cpp:589`). Issue #13
reports visible blocking on the official GTA VI trailer. Merserk now offers
ProRes HQ and FFV1 10-bit; video2dlssnr defaults to HEVC 10-bit at CQ19.

**Do** — P010 capture with HEVC Main10, a CQ ladder, and a lossless rung
(FFV1 or NVENC lossless), all in the cache key. Score every rung with VMAF
against a lossless intermediate (libvmaf_cuda), and print the cost beside
each, as the NVENC preset tooltip already does. Main10 is also the first half
of P3.1.

**Impact** — Pipeline: fixes the one quality complaint users have filed
about export.

---

<a id="p24"></a>
### P2.4 · Guide A/B harness, then evaluate Video Depth Anything

`S` harness · `M-L` to ship a model · **Pipeline**

Add `depth=file:` / `mv=file:` guide modes, so depth and flow computed offline
in Python can be A/B'd in `tools/benchmark` without shipping a model. Add
near/far test clips; your reply on issue #8 sets exactly that bar.

The candidate is **Video Depth Anything Small**: Apache-2.0 (Base and Large
are non-commercial), 28.4M parameters, about 7.5 ms per frame at 518².
- **Normalise over the clip, not per frame.** Its inverse depth must use a
  stable scale, or depth will pump.
- **Map to the convention `DLSSBackend.cpp:255` expects**: 0 = near, 1 = far.
- **Deployment:** ONNX Runtime's TensorRT-RTX execution provider, which
  covers RTX 30 and newer.

Refs: [Video-Depth-Anything](https://github.com/DepthAnything/Video-Depth-Anything) ·
[TensorRT-RTX EP](https://onnxruntime.ai/docs/execution-providers/TensorRTRTX-ExecutionProvider.html)

**Impact** — Pipeline: the deciding test for better depth and flow, and a
prerequisite for P3.6.

---

<a id="p25"></a>
### P2.5 · Temporal stability with motion compensation

`M` · **Pipeline, Player** · _supersedes old 3.6_

The old plan ("one history texture, one lerp") ghosts on anything that moves.
The player already computes a reverse flow field and a round-trip trust test
(`FlowGate.h`).

**Do** — warp the previous neural frame by that flow, blend only where the
flow is trusted, and reset on `ClassifySceneCut`. Tune the default on the
benchmark's added-sigma metric. It ships as a ladder with Off available,
because this is quality-affecting.

This is the defining failure mode of neural video: Merserk ships shimmer
suppression, and Topaz has said on the record that it has no deflicker.

Refs: [Lai et al., ECCV 2018](https://arxiv.org/pdf/1808.00449)

---

<a id="p26"></a>
### P2.6 · Spatial mask and feather; face protection later

`S-M` · **Pipeline, Player** · _old 3.3_

Neural strength is already a per-frame blend in the presentation shader, so
making it vary across the frame is one texture and a lerp, done in *our*
compositor (NGX mask inputs are inert, `docs/ARCHITECTURE.md`).

**Do** — ship a manual mask with feathering first. Face and skin protection
is now the loudest criticism of DLSS 5 ("beautified" faces), and NVIDIA's own
answer is masking. A ComfyUI pack already ships an automatic skin mask, and
Merserk has an open mask issue (#65). Detection is a separate dependency;
defer it.

---

<a id="p27"></a>
### P2.7 · Processing scale

`S-M` · **Pipeline** · _old 3.5_

A selector for the resolution the model runs at, independent of the output,
and a cache-key term. It goes straight at the "4K30 could not keep up" limit.
**The default is Source / 100%.** Below 100% is an explicit, labelled rung
with its measured cost; above 100% is offered for more quality.

---

<a id="p28"></a>
### P2.8 · RTX Video Super Resolution as a second engine

`M` · **Pipeline, Player** · _old 3.4_

RTX VSR is trained on compressed video, which is the right model for YouTube
sources, and the RTX Video SDK supports D3D12. VSR itself is now common;
**what nobody else can show is the original, DLSS 5 NR and RTX VSR on the
same frame during a live render.** At 1× it also doubles as a
compression-cleanup pre-pass.

Ref: [RTX Video SDK](https://developer.nvidia.com/blog/enhancing-low-resolution-sdr-video-with-the-nvidia-rtx-video-sdk/)

---

<a id="p29"></a>
### P2.9 · Super Resolution-only export

`M` · **Pipeline** · _old 3.0_

Super Resolution on its own is the only one of the seven export combinations
the player refuses. The helper always runs `ConfigureNeuralAddon(ini, true)`,
and `OfflineNeuralRenderer` requires feature 18 to be armed before capture.

**Do** — pass `requireNeural` into `ConfigureNeuralAddon`, which forces a
helper relaunch because ReShade reads its ini at load. Gate the arming check
and the priming loop on it, and report the neural evidence honestly when it
is off. `ExportMatrixSmoke` asserts today's byte-equality, so remove that
assertion together with `ExportRefusal::UpscaleNeedsNeural`.

---

<a id="p210"></a>
### P2.10 · CLI / headless invocation

`S` · **Pipeline** · _old 3.10_

`DLSSVideoPlayer.exe --input X --range A-B --preset Y --stages sr,nr,fg --out Z`

This is the cheapest route to batch work without building a queue UI, and it
makes the benchmark harness a first-class consumer. Merserk's issue #64 asks
for exactly this chain (upscale → interpolate → enhance → export), which
**Export with DLSS stages** already runs.

---

<a id="p211"></a>
### P2.11 · Quality metrics in the app

`M` · **Player, Pipeline** · _old 3.11, expanded_

PSNR and VMAF against the source penalise the very change the user asked for.
Measure what the model did to **motion** instead:
- **tOF** — the difference between the output's flow and the source's flow;
  NVOFA can compute it in the app.
- **Warping-error delta** — how much more the output flickers than the
  source did.
- **Temporal sigma** — already computed in `tools/benchmark`.

Show them next to a blind A/B for the current settings on the current clip.
For offline work, add CGVQM and ColorVideoVDP error maps to `tools/benchmark`.

Refs: [CGVQM](https://github.com/IntelLabs/cgvqm) ·
[ColorVideoVDP](https://github.com/gfxdisp/ColorVideoVDP)

---

<a id="p212"></a>
### P2.12 · Scene-cut controls and duplicate-frame handling

`S` slider · `M` dedup · **Pipeline** · _old 3.12_

Expose the cut threshold that is already computed, and ship a defensible
default from `cutlab.py --sweep`. SVP users ask both for more aggressive cut
detection and for none at all, so one fixed value satisfies nobody. Animation
drawn on twos and threes needs duplicate-frame handling before frame
generation.

---

<a id="p213"></a>
### P2.13 · Re-measure which settings change the image on RenoDX 6.5.3

`S` · **Pipeline**

`docs/BENCHMARK.md`'s table of which controls change the image was measured
on RenoDX 4.70, and it still decides which controls are hidden. Community
tools now expose `SkinStructure`, `AutoMask` and `UICorrection` as live
controls. Re-run the table on the pinned runtime. It may also give P2.6 its
face protection for free.

---

<a id="p214"></a>
### P2.14 · A deband pre-pass for compressed sources

`S-M` · **Pipeline**

A libplacebo-style deband (1 iteration, threshold 3, radius 16, grain 4)
before the model, off by default until measured. It is cheaper than running
RTX VSR at 1× as a cleanup step. Measure first whether the neural pass
amplifies banding; P2.11's metrics are the tool.

## Player UX

<a id="p215"></a>
### P2.15 · An on-screen compare bar with press-and-hold A/B

`S` · **Player**

Comparison is the product, and today it sits behind Video > Compare. The
split and wipe modes draw no labels (`D3D12Renderer.cpp:284`). Neural strength
is the seventh slider in Image adjustments, and it overlaps conceptually with
Compare > Blend.

**Do** — a compact bar holding mode, blend or strength, zoom and swap, with
"Original" and "DLSS 5" labels drawn on the image. Holding the mouse on the
picture shows the original, as Topaz and NVIDIA ICAT do. Merge strength and
blend into one control.

---

<a id="p216"></a>
### P2.16 · Zoom, pan and a synced magnifier

`M` · **Player**

Today there is a single 2× toggle, anchored where it was pressed, with no pan
(`main.cpp:3058-3065`). Add 1:1, 2×, 4× and 8× zoom, wheel zoom at the cursor,
drag to pan, and a loupe that shows the same spot on both sides. Pixel-peeping
faces is the core use; see [video-compare](https://github.com/pixop/video-compare)
and [Improve-ImgSLI](https://github.com/Loganavter/improve-imgsli). It depends
on P0.10 for a true 1:1.

---

<a id="p217"></a>
### P2.17 · A difference view

`S-M` · **Player**

`|neural − original|` as amplified luma or colour, as one more mode in the
existing compare shader, optionally with an SSIM map. It answers *where did
the model change the picture*, which is the question visitors ask, and it is
the natural authoring view for P2.6's masks.

---

<a id="p218"></a>
### P2.18 · The timeline as a render map

`S` band · `M` thumbnails · **Player**

Progressive whole-video coverage is the project's real advantage, yet on
screen it is a thin teal line (`neural-playback.jpg`).

**Also** — when the duration is unknown (browser-recorded WebM), grey out
the timeline: seeking itself works since P1.6, but the bar shows nothing.

**Do** — a thicker coverage band with a hatched "rendering now" segment at
the render head, the time on hover, time-to-full-coverage, and chapter
markers. Then hover thumbnails: the cached neural frame where one exists,
otherwise the original. uosc with thumbfast is the model.

---

<a id="p219"></a>
### P2.19 · Status chips instead of one overflowing line

`S` · **Player**

`BuildPlayerStatusText` (`UiLayout.cpp:526`) is trimmed for width and still
overflows ("…Frame Generat…" in `neural-playback.jpg`). The three feature
buttons are 264-270 dip wide each (`UiLayout.cpp:29-50`), which forces a
minimum width of about 1,070 dip.

**Do** — fixed chips (`Render 16% · ETA`, `fps`, `Dropped`) that flash briefly
when a value changes, and narrower buttons.

---

<a id="p220"></a>
### P2.20 · A start screen with a capability check and tiles

`M` · **Player**

The idle screen offers only Open file and Open YouTube URL
(`player-start.jpg`).

**Do** — show the GPU, the driver against the 610.47 minimum, whether the
runtime is present and its lock state, and a predicted render speed at 1080p
and 1440p; offer safe mode when a check fails. Below that, tiles for game
trailers and recent videos, each with a cached neural frame and a coverage
badge, plus the `D` hint. The preset strip previews its four presets on the
paused frame; they all measured the same cost (6.3 s). This answers issue #1
("what resolution do I have to give to this?").

---

<a id="p221"></a>
### P2.21 · Export the comparison itself

`S` PNG · `M` clip · **Player**

Save the composed split exactly as shown, with labels and a provenance footer
(frame, settings digest, runtime), and later a short wipe clip. The demo video
is assembled by hand today (`docs/media/README.md`). This turns every user
into a source of verifiable evidence.

---

<a id="p222"></a>
### P2.22 · Dark, DPI-aware menus and dialogs

`M` · **Player**

The title bar is dark (`main.cpp:1268`), but the menu bar and the dialogs
are light Win32 classics. The dialogs use raw pixel positions, the 96-dpi
`DEFAULT_GUI_FONT` and an unscaled client size (`main.cpp:3291`, `:3390`), so
they are tiny at 200%. Scale the dialogs to DPI first; that is a correctness
fix. Then darken them, and draw the menu bar via `WM_UAHDRAWMENU` (an
undocumented API; guard it).

---

<a id="p223"></a>
### P2.23 · Keyboard discoverability

`S-M` · **Player**

About 25 shortcuts are spread across 5 menus. Add a `?` cheat-sheet overlay
first, and a searchable command palette later (as uosc and Improve-ImgSLI do).

---

<a id="p224"></a>
### P2.24 · Media controls and taskbar buttons

`S-M` · **Player**

Taskbar progress already exists (`ITaskbarList3`, `main.cpp:2279`). Add System
Media Transport Controls via
[`ISystemMediaTransportControlsInterop`](https://learn.microsoft.com/en-us/windows/win32/api/systemmediatransportcontrolsinterop/nf-systemmediatransportcontrolsinterop-isystemmediatransportcontrolsinterop-getforwindow),
and thumbnail-toolbar buttons for play/pause, neural on/off and compare
(at most 7, fixed when created).

---

<a id="p225"></a>
### P2.25 · Synced multi-pane comparison

`M-L` · **Player** · _pays off with P2.8_

Side-by-side and 2×2 layouts, for example Original | NR | NR at another
strength | RTX VSR, all on one timestamp. NVIDIA ICAT supports four synced
inputs. This is the view that turns P2.8 into something no other tool offers.

---

<a id="p226"></a>
### P2.26 · A comparison gallery on the site, plus site fixes

`M` · **Site**

- **Gallery** — several scenes (a face, a photo, a low-bitrate clip, and one
  honest failure case), each with an A/B flip, a 1:1 loupe, and its own link
  and social card. The site shows one hero slider and one video today. A flip
  is a state change, so it stays within the design system's One Moment rule.
- **Fixes**
  - The two 1920×1080 hero JPEGs (295 KB and 255 KB) are preloaded on every
    viewport, with no `srcset` or AVIF/WebP (`site/src/index.html:30-31`,
    `:133-137`).
  - The fixed full-viewport SVG noise layer sits at z-index 60
    (`styles.css:107-114`).
  - The comparison slider has no `aria-valuetext` (`index.html:149`).
  - The skip link targets `#download` instead of the main content.

---
---

# P3 — Strategic or large

<a id="p31"></a>
### P3.1 · HDR end to end

`L` · **Pipeline, Player** · _old 3.13_

**Blocked today:**
- the swapchain is `R8G8B8A8_UNORM` (`D3D12Renderer.cpp:229`), with no
  `SetColorSpace1`;
- `colorBuffersHDR = false` is hardcoded (`DLSSGBackend.cpp:410`);
- the cache is 8-bit.

**The traps specific to neural processing:**
1. **Auto-exposure over PQ is meaningless.** Do P2.1 first.
2. **Per-frame peak detection flickers once frame generation is added.**
   Smooth the peak, as libplacebo does with `peak_smoothing_period`.
3. **A tone-mapped cache is tied to one display.** Cache scene-referred
   output, or key the cache on the display's parameters.
4. **NVIDIA's own video path took several driver generations to reach HDR.**

**Approach:**
- Detect HDR with `IDXGIOutput6::GetDesc1`.
- Output `R10G10B10A2` with `SetColorSpace1(RGB_FULL_G2084_NONE_P2020)`.
- Match SDR white via `DISPLAYCONFIG_SDR_WHITE_LEVEL` (203 nits per BT.2408).
- Do not call `SetHDRMetaData`.
- **Never toggle the OS HDR setting.**

P2.3's Main10 export is the first half of this task.

---

<a id="p32"></a>
### P3.2 · Subtitles via libass, composited after the network

`M-L` · **Player** · _old 3.7_

A player that cannot show subtitles pushes people to export and watch
elsewhere, which undercuts the whole idea of watching while it renders.

**Composite after the network.** Subtitles are HUD, and the model warps and
shimmers static text.

**Details that are routinely got wrong:**
- `ass_set_storage_size` is mandatory;
- use `ASS_FONTPROVIDER_DIRECTWRITE`, plus `ass_add_font` for fonts embedded
  in MKV attachments;
- render at output resolution, not video resolution.

**Formats:** SRT, ASS/SSA, PGS, VobSub, WebVTT and `mov_text`.

**Baseline UX:** delay adjustment, switching track mid-play, loading external
files automatically, and detecting the text encoding.

---

<a id="p33"></a>
### P3.3 · WASAPI drift correction and passthrough

`M` · **Player** · _open parts of old 3.8_

- **Drift correction**: resample with `swr_set_compensation` against the
  `IAudioClock` error. Nothing corrects a crystal offset over a full-length
  film today.
- **Bitstream passthrough**, behind a toggle, via FFmpeg's `spdif` muxer.

**Design against two known bugs:**
- mpv #1773: `IAudioClient::Release` can hang after a format change;
- Kodi #18453: a display-mode change drops the HDMI audio sink.

---

<a id="p34"></a>
### P3.4 · Extract testable units from `main.cpp`

`M` · **Player** · _old 3.14_

`main.cpp` is now 7,852 lines, and `PlayerApp` holds about 83% of it. `Tick`,
`Position` and `PerformSeek` depend on the concrete decoder, audio and renderer
members, which is why P0.2 and P1.4 cannot be tested without hardware.

**Do** — follow the existing `*Policy.h` pattern, not a new abstraction layer.
Start with `ClampSeek` (`main.cpp:4480`), the render-pace load/save, and
`ParseArgs`. Then split `StartNeuralJob` (`:6258`) and
`OfflineNeuralRenderer::RunJob`.

---

<a id="p35"></a>
### P3.5 · Prefer NVIDIA's signed runtime on RTX 50

`M` · **Pipeline, Release** · unverified

Driver 616.64 enables DLSS 5 officially on RTX 50 and downloads its models to
`%ProgramData%\NVIDIA\NGX\models`. If a usable signed neural-rendering DLL can
be found there for an app that is not on NVIDIA's list, preferring it on RTX
50 would remove the "modified, unsigned" warning for those users. Record
which runtime ran in the receipt. Keep the community build for RTX 20-40.
**Nobody has yet confirmed the signed DLL is usable this way.** Check that
before planning any work.

---

<a id="p36"></a>
### P3.6 · Neural optical flow as an export-only rung

`M-L` · **Pipeline** · _gated by P2.4_

SEA-RAFT's smallest model runs 1080p at about 21 fps on a 3090, and an ONNX
export exists. That is too slow for live playback, but acceptable for export.
Keep NVOFA for live. Build this only if P2.4's harness shows a gain: motion
vectors measured only +0.297 dB on the cuts-motion clip.

---

<a id="p37"></a>
### P3.7 · Feed NVENC directly from D3D12

`M` · **Pipeline** · _a narrow form of the parked zero-copy rewrite_

Keep the ffmpeg child for decoding and codec coverage, but encode the cache
straight from the D3D12 texture. NVENC has accepted D3D12 input with fence
synchronisation since SDK 11.1. This removes the readback and pipe copy on
the busiest path. Do it only if P2.7 and P1.x still leave 4K30 short of real
time.

---

<a id="p38"></a>
### P3.8 · Repository media hygiene

`S` · **Release**

- **Media in history:** `docs/media/neural-comparison-demo.mp4` is stored
  three times (9.1, 7.9 and 7.2 MB). There is also a 5.3 MB benchmark fixture
  and a 4.2 MB webp, and the pack is 60 MB.
- **Stray build output:** a 775 MB `build/` directory sits in the tree
  (gitignored, but still there).

Serve media that can be re-shot from release assets or LFS, and run `git gc`.
Delete the stray `build/`.

---
---

# Parked

Researched and deliberately **not** being done. They are recorded here so they
are not proposed again.

| Item | Why not |
| --- | --- |
| **Custom model loading (.pth/.onnx)** | Structurally impossible: NGX feature 18 has fixed weights. chaiNNer-style tools are a different category. |
| **Watch folders** | Requested at Topaz since 2022 and never shipped, with no sign that users urgently need it. P2.10's CLI covers the batch case. |
| **Stabilization, deinterlacing, colorization, face restoration** | Each needs a model we cannot obtain. Inferior versions dilute a single-model product. |
| **8K/16K output, 480 fps frame generation** | Marketing checkboxes that no panel can show. Cadence-aware multiple selection is the right design, so say that instead. |
| **Cloud rendering / credits / tiering** | Being MIT and free is an asset against a closed competitor. |
| **Linux or mobile ports, multi-GPU splitting of one render, "auto" model recommendation** | Feature 18 on Windows D3D12 is the product. Separate GPU choices for AI and for NVENC is the useful form (`GpuPreference.cpp`). |
| **Full zero-copy decode/encode rewrite** | The ffmpeg child is load-bearing: codec coverage, process isolation for the runtime lock, and the benchmark harness. Only the narrow encode side is worth doing (P3.7). |
| **AV1 export** | Needs a 40-series NVENC. HEVC Main10 (P2.3) covers the need everywhere. |

---
---

# What is already strong

Verified in the 2026-09-20 and 2026-09-23 audits. **Do not "fix" these, and
do not suggest them again.**

**Architecture and protocol**
- **The IPC wire format:** magic number plus an exact version gate,
  `static_assert` on every struct size, and decoders that reject trailing
  bytes, oversized counts and overflow. One shared header rather than two
  schemas.
- **No reachable command injection:** no `cmd.exe`, `system()` or `_popen`.
  `lpApplicationName` is always a handle-verified absolute path. yt-dlp runs
  with `--no-config --no-plugin-dirs`, and URLs are allowlisted and filtered.
- **Helper TOCTOU is closed:** `FILE_FLAG_OPEN_REPARSE_POINT`, canonicalised
  through the same handle, which stays open with `FILE_SHARE_READ` across the
  whole resolve.
- **No cache path is ever read from disk.** Every path is
  `root/<bucket>/<hex-digest>`, re-checked with a component-wise descendant
  test.
- **Publication is a directory rename**, and SHA-256 covers the payload and
  both sidecars on every read.
- `SegmentWriter`'s three-thread handshake, `CompletionRegistry`'s scalar
  tokens, and `ParallelFor.h`'s pool have each been walked for every
  interleaving and hold.

**GPU**
- **D3D12 slot discipline:** a slot wait before every mapped write and
  allocator reset, and a drain before releasing DLSS.
- **DRED is wired up.**
- **GDI objects are always released**, including on failure paths.
- NVDEC runs on both the playback and offline paths.

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
- **SHA-256 has exactly one implementation.** That is the pattern P1.16
  should copy.
- **The update check** uses `WINHTTP_FLAG_SECURE`, bounded timeouts, a
  body-size cap, and downgrade protection.
- **Zero /W4 warnings**, `#pragma once` everywhere, no `#if 0`, and no
  commented-out code.

`docs/TROUBLESHOOTING.md` quotes literal log lines and tells the user how to
read them. Keep that standard. The docs' menu paths and shortcuts match
`AppMenu.cpp` and `main.cpp`, and every repository path they cite exists.

---

## Method

**2026-09-23, against 23d71d9.** Five agents ran in parallel, then the lead
re-read the top findings (✅) against source:

| Agent | Scope |
| --- | --- |
| Playback audit | `main.cpp`, `SynchronizedPlayback`, audio/WASAPI, `D3D12Renderer`, `VideoDecoder`, `MediaPipeline`, UI |
| Neural audit | Helper, IPC, `OfflineNeuralRenderer`, cache, guides, frame generation, runtime lock, YouTube |
| Build/CI/docs | Built Release, ran all 26 CTest tests and the site tests; reviewed CMake, workflows, packaging scripts, docs drift, site, repository size |
| Pipeline research | The DLSS 5 ecosystem as of September 2026, depth and flow models, libplacebo/mpv, RTX Video, NVENC, neural-video metrics |
| UX research | ICAT, video-compare, Improve-ImgSLI, Topaz, uosc/mpv, Windows shell integration, and the current UI, screenshots and site |

Everything open on the 2026-09-20 list was re-checked against current source
and carried over, with updated line numbers, or dropped once done.
