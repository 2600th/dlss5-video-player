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
| **P1** | | | | |
| **P2** | | | | |
| [P2.1](#p21) | Supply a smoothed exposure instead of auto-exposure | S | Pipeline | ✅ |
| [P2.4](#p24) | Evaluate Video Depth Anything through the guide harness | M-L | Pipeline | |
| [P2.8](#p28) | RTX Video Super Resolution as a second engine | M | Pipeline, Player | |
| **P3** | | | | |
| [P3.4](#p34) | Extract testable units from `main.cpp` | M | Player | |
| [P3.5](#p35) | Prefer NVIDIA's signed runtime on RTX 50 | M | Pipeline, Release | |
| [P3.6](#p36) | Neural optical flow as an export-only rung | M-L | Pipeline | |
| [P3.7](#p37) | Feed NVENC directly from D3D12 | M | Pipeline | |
| [P3.8](#p38) | Repository media hygiene | S | Release | |

---
---

# P0 — Fix now

---

# P1 — Next

## Player: reliability and quality-neutral performance

## Pipeline: helper, cache, runtime

## Release and CI

## Docs and site

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

<a id="p24"></a>
### P2.4 · Evaluate Video Depth Anything through the guide harness

`M-L` · **Pipeline** · _the harness half shipped: `depth=file:` / `mv=file:` guide modes_

The harness is in: offline depth and flow can be fed to the worker from
files and A/B'd through `tools/benchmark` (see its README for the exact
steps), with near/far clips `depth-pan` and `depth-subject`. Feeding the
built-in guides back through files is byte-identical.

**What is left** needs torch and a model download, so it is the owner's to
run: generate Video Depth Anything Small depth for the corpus, normalised
over each clip (not per frame), and A/B it. The first harness result sets
expectations: even *exact* synthetic depth moves the output by at most
0.03 dB PSNR, so depth has little headroom on this pipeline; flow is the
more promising guide (P3.6).

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

## Player UX

---

# P3 — Strategic or large

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
