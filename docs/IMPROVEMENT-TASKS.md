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
| [P2.2](#p22) | Dither wherever the image is cut to 8 bits | S | Pipeline, Player | ✅ |
| [P2.3](#p23) | A quality ladder for cache and export: CQ, 10-bit, lossless | M | Pipeline | ✅ |
| [P2.4](#p24) | Guide A/B harness, then evaluate Video Depth Anything | S / M-L | Pipeline | |
| [P2.8](#p28) | RTX Video Super Resolution as a second engine | M | Pipeline, Player | |
| [P2.13](#p213) | Re-measure which settings change the image on RenoDX 6.5.3 | S | Pipeline | |
| [P2.14](#p214) | A deband pre-pass for compressed sources | S-M | Pipeline | |
| **P3** | | | | |
| [P3.1](#p31) | HDR end to end | L | Pipeline, Player | |
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
