# DLSS 5 Video Player — Updated Roadmap

_Current as of September 15, 2026._

## Quick reality check

- DLSS 5 Neural Rendering is officially available on RTX 50-series GPUs, but NVIDIA's public DLSS repository still lists SDK 310.7. Video processing through Feature 18 remains an experimental community workflow. [NVIDIA announcement](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [DLSS 5 research](https://research.nvidia.com/labs/adlr/DLSS5/) · [Public SDK releases](https://github.com/NVIDIA/DLSS/releases)
- Keep the player's currently tested stack pinned: **driver 616.64 + ReShade 6.8 + RenoDX 4.70 + NR/SR 310.8**. [Runtime lock](../packaging/runtime-lock.json) · [Measured report](measurements/runtime-comparison-20260907/REPORT.md)
- **Tested against that pin (2026-09-12):** DLSS5-Autopilot's field data reports renodx-dlss5 4.6/4.7 faulting on every evaluate from driver 616.64, which is the driver we recommend. Six live neural sessions on an RTX 5090 on 616.64 did not reproduce it: `failure=none`, `lock=ok`, every rendered frame verified. The pin stands. See item 0 below. [Session record](VERIFICATION-2026-09-12-RTX5090.md)

## The 2026-09-11 survey — status

Written after v0.20.0 shipped hardware optical flow, and kept as that survey's
record: every item was checked against the source or an external document on that
date, ordered by value over cost. Items 1-4 were four flags and about thirty lines
aimed at one symptom - motion that does not feel attached to the picture. Item 0
closed 2026-09-12; items 1, 2 and 4 shipped 2026-09-14; item 3 was measured the
same day and not adopted; item 5 is what remains.

**0. Closed 2026-09-12: it does not reproduce on this pin.** DLSS5-Autopilot's
aggregated field data reports renodx-dlss5 4.6/4.7 faulting on every evaluate from
driver 616.64 - exactly the combination we pin and recommend, while their
workaround was a standalone route that never loads the add-on. Six live neural
sessions on one RTX 5090 on 616.64 with the pinned stack (ReShade 6.8.0.2155,
RenoDX 4.7, DLSS-NR 310.8.0, `310.8.SF-v2`) answered it: `failure=none`,
`lock=ok`, every rendered frame verified (`2805/2805`, `2779/2779`, `2697/2697`,
`2607/2607`) at about 7.0 ms/frame at 1920x1080, and no evaluate faulted. Not
reproduced here is not the same as wrong - their figure aggregates machines,
add-on builds and titles one session cannot speak for - so the pin stands, and so
does the warning against upgrading the runtime blind: what has been tested is
616.64 with renodx-dlss5 4.70, and nothing else.
[Autopilot v1.8.1](https://github.com/Kizzuwatnaa/DLSS5-Autopilot/releases/tag/v1.8.1) ·
[Session record](VERIFICATION-2026-09-12-RTX5090.md)

**1. `NV_OF_PRED_DIRECTION_BOTH`.** `src/OpticalFlowNvof.cpp` asks for forward flow
only. The NVOFA guide: "When `NV_OF_INIT_PARAMS::predDirection` is set to
`NV_OF_PRED_DIRECTION_BOTH`, forward and backward flow will be generated in a single
`NvOFExecute`/`NvOFExecuteD3D12`/`NvOFExecuteVk` API call", with `bwdOutputBuffer`
and `bwdOutputCostBuffer` alongside. A forward/backward disagreement test is
measured in pixels, so unlike the cost gate it needs no invented constant; the
standard criterion is scale-free. This closes two open items at once and does not
need a second session or a second Execute.
[NVOFA guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html)

**Shipped 2026-09-14**, with the fallback the item did not anticipate: the engine
is asked for both directions and global flow and gives up one capability at a
time rather than the whole engine, because a refused `nvOFInit` cannot be
retried in place. The resolve pass gates each vector on the round trip
(`src/FlowGate.h`, Sundaram/Brox alpha 0.01, beta 0.5 px², literature defaults),
so a cell the engine contradicts itself about emits no motion.

**Then settled in three stages, and the last one reverses the first.** On four
synthetic clips (2026-09-14, RTX 4080 SUPER) the gate moved no metric by as much
as one percentage point and *raised* false motion by 0.92 points on the only clip
with genuine disocclusion, so it was kept on the shape of its risk rather than on
evidence [session record](VERIFICATION-2026-09-14-RTX4080.md). On four clips cut
from this repository's own NR-processed demo capture it **lowered** false motion
on all four by 4.6 to 14.4 % relative - 11.8 to 74.4 % of the share the
`intensity-0` control attributes to the neural pass - and improved flips and
added sigma with it on two, an outcome no synthetic clip produced
[A/B report](measurements/gate-real-footage-20260914/REPORT.md). On 2026-09-15 it
reproduced on the seven `orig-*` clips cut from publishers' own releases, with the
gate isolated to its *rejection* alone: false motion better on **6 of 7** by
2.11 % to 46.91 % relative, cell flips 7 of 7, added flicker 7 of 7, 50 renders
each bit-reproducible within its arm to full precision. That retires the
provenance caveat, and leaves the synthetic reversal as this project's clearest
case that synthetic patterns can point the wrong way on direction
[camera-original report](measurements/q1-gate-camera-original-20260915/REPORT.md).

Two limits travel with the win. The estimator under test was NVOFA hardware flow
on all seven clips, so what those numbers measure is the **resolve-shader** half:
`kRoundTripCells` in the CPU estimator never decided a motion texture, and
`BuildDepthProxy` still reads the CPU flow field, which a depth=0 probe bounds at
0.3-1 pp relative.

**2. `enableGlobalFlow`.** Also off today, also computed inside the Execute we
already issue: "a global flow vector is estimated from forward flow in the same
`NvOFExecute` ... API call". A slow pan is a large, coherent global vector the
per-cell field agrees with; a cut is one it does not. Our cut test currently has no
motion evidence in it at all.

**Shipped 2026-09-14.** Asked for alongside both directions, on the same ladder,
and read back as a fence-latched four-byte copy that never stalls the frame.
Nothing consumes it yet: feeding it into the cut decision changes a shipped
judgement, so it waits for the harness numbers rather than landing unmeasured.

**3. The zero-motion SAD we already discard.** `EstimateFlow` records the
zero-displacement cost and drops it - only `bestGlobal` escapes as `globalCost`.
x264/x265 do not threshold an absolute residual, they compare inter cost against a
no-prediction baseline, which is scale-free where our fixed 0.10 band is not. Our
own note records the softest observed real cut at 0.108, sitting on that threshold.
mvtools adds the second half of the same idea: decide on the *fraction* of blocks
that failed, not the mean. Per-cell confidence already exists; the fraction is one
accumulate.
[x265 scenecut-bias](https://x265.readthedocs.io/en/stable/cli.html) ·
[mvtools thSCD2](https://avisynth.org.ru/mvtools/mvtools2.html)

**Measured 2026-09-14, not adopted.** Implemented in `tools/benchmark/cutmirror.py`
and swept over 1350 threshold points against the shipped residual family's 198, on
nine labelled clips and 1212 consecutive pairs. Both families reach the identical
best operating point - every labelled cut found, no over-reset, the same two false
positives on the same clip, F1 0.875 - so the per-cell state the fraction needs in
`EstimateFlow` would buy nothing. Neither score separates the set on its own: the
weakest true cut is residual 0.1853 against a 0.3739 non-cut, and failed fraction
0.5661 against a 0.7297 non-cut. Both families are carried by the two-arm split and
the debounce, not by the score. The sweep and the reasoning are in
`docs/BENCHMARK.md`; what the shipped criterion gets wrong - one over-reset on
`cuts-motion`, every cut between shots that share a luma histogram, and a flash
taken for a cut - is now labelled corpus, so the next attempt starts from a set
rather than from a clip.

**4. Temporal metrics in the harness.** `tools/benchmark/analyze.py` has no
per-pixel temporal variance, no false-motion rate and no cut precision/recall. The
60.8 % -> 3.7 % false-motion figure and 0.20.0's 2.23 -> 1.41 temporal sigma are both
prose from ad-hoc runs that no script reproduces. The ground truth for cuts is
already written: `tools/benchmark/corpus.py` records hard-cut frame indices in the
manifest and `analyze.py` only uses them to exclude frames. Until this exists, every
item below is unfalsifiable, which is also why the reference table being stale
matters more than it looks.

**Shipped 2026-09-14.** `analyze.py` now scores per-pixel temporal sigma inside a
shot with its p99, the false-motion rate over the cells the source held static,
the cell flip rate of the motion field, and cut precision/recall/F1 against the
manifest's hard-cut indices at ±1 frame, plus a guide A/B table and a per-firing
cut table carrying each decision's residual and histogram overlap. The detector
it scores against mirrors `TemporalGuides.cpp` - same grid, same cell luma, same
0.30/0.10/0.85 thresholds, same 0.6 s debounce - so a threshold swept here
transfers without a second calibration, and item 3 above is now falsifiable. The
2026-09-08 reference table predates hardware optical flow and these metrics and
is marked stale until it is re-run.

**5. Then, now that they can be measured:** re-run the reference table; A/B `IsHDR`
on the linear FP16 input; A/B a supplied 1x1 exposure texture against auto-exposure;
A/B constant depth against the proxy; calibrate the NVOFA cost gate against the
forward/backward mask from item 1 and keep it as the cheap one-pass runtime proxy if
agreement is high.

### Corrections this survey produced

- **Temporal hints.** We disable them unconditionally. NVOFA guideline 3: "Disable
  the temporal hints only if there is a-priori knowledge of no temporal correlation
  (e.g. a scene change, independent successive frame pairs)." The shape NVIDIA
  describes is on within a shot, off at a discontinuity - which we already detect.
  The real cost of changing it is cache determinism, since output would then depend
  on where the segment started. A design decision, not a bug.
- **Auto-exposure is not a defect.** Streamline: "If `sl::kBufferTypeExposure` is NOT
  provided or `dlssOptions.useAutoExposure` is set to be true then DLSS will be in
  auto-exposure mode". Our null texture is the sanctioned path. Re-scope the item
  from "fix" to "never measured against a supplied exposure".
- **Depth is not in any official description of the model's inputs.** NVIDIA
  Research: "the model is conditioned on the current rendered frame, engine motion
  vectors, carried temporal state, and artistic-direction values." Depth appears
  nowhere. Our SR carrier is a real SR feature and does consume depth, so it cannot
  simply be deleted - but the useful experiment is constant-depth versus the proxy,
  not more tuning of the per-frame normalizer.
- **NVOFA cost has no published scale.** Three official sources say only that a
  higher cost means a less accurate vector. No range, no normalization, no
  recommended cutoff. The gate cannot be closed by reading; it has to be measured or
  replaced by item 1.
- **Hardware flow does not cover upscaling sessions.** `D3D12Renderer.cpp` only
  offers the engine when the decoded frame is already the DLSS input size, so the
  runtime SR toggle silently keeps the CPU estimator. True and deliberate, but only
  ARCHITECTURE.md says so.
- **The driver floor has an official citation now.** The 616.64 release notes name
  it for DLSS 5 Neural Rendering, so `src/RuntimePolicy.h` can stop citing a
  community figure in text that reaches a dialog.
- **Frame generation for video is now an NVIDIA product**, in the Video Effects SDK
  rather than DLSS, and it ships automatic shot-change detection with a hard bypass
  on by default. That both decides the Frame Generation stub (point at VFG or delete
  it) and independently confirms that a cut detector plus a bypass is the accepted
  answer to our problem.
  [VFG filter](https://docs.nvidia.com/maxine/vfx/latest/Filters/VideoFrameGeneration.html)

### Where the ecosystem actually is

Nobody has solved temporal coherence for video neural rendering. dlss5-bridge says
of its own substitute contract that "text softens and dense foliage smears";
DLSS5-Feeder tells users to "expect the temporal quality of estimated motion vectors
(some ghosting in fast motion, softness on thin moving geometry)". video2dlssnr is in
the identical state as us on the confidence gate, with the comment "Confidence gate
disabled (costLo==costHi==0 -> shader keeps every vector)". No project in the survey
implements a forward/backward check, and no one has published cost thresholds - that
ground is unclaimed.

NVIDIA has still published no neural-rendering API: the public NGX header names
feature 18 `NVSDK_NGX_Feature_Reserved18`, and Streamline 2.14.1 shipped five days
after DLSS 5 launched with no NR plugin or guide. Nothing states that video is or is
not a supported use. Every ordering and threshold decision here is ours to measure;
there is no spec to defer to.

## The persistent render helper — built and accepted 2026-09-14

**Why it existed.** After the startup work cut the toggle from 14.71 s to 9.24 s
on a scanned install and from 11.43 s to 5.80 s on an excluded one
([record](VERIFICATION-2026-09-12-RTX5090.md)), what was left was not duplicated
work but a cold process: every session spawned `NeuralWorker.exe` and paid
CreateProcess plus the antivirus scan of a 98 MB tree, ReShade proxy init, NGX
init against a model cache the driver never created, `CreateFeature` and the first
evaluate, and only then the first segment's preroll, encode and mux - around a
render of 60 frames at 7.0 ms. Every amortisation the vendors document is
process-scoped, so no flag could reach it: DLSS's cached feature memory "gets
released when Shutdown() is called"
([programming guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf)),
and NVENC session open was measured at 953-1172 ms per instance on NVIDIA's own
forum
([thread](https://forums.developer.nvidia.com/t/nvenc-performance-issues-when-creating-multiple-encoders/44902)).
Every sibling project converged on the same shape
([Ecosystem review](ECOSYSTEM_REVIEW.md)). Instrumentation on 2026-09-14 then put
the premise on a number: **NGX init plus feature arm is 95 % of the helper's cold
start, and all of it is per-process**
[session record](VERIFICATION-2026-09-14-RTX4080.md).

**The acceptance criterion changed once, on measurement.** Ten driven sessions put
toggle-to-first-neural-frame at 8.39-9.18 s on a cleared first toggle and
4.88-5.16 s warm, and the phase split showed residency could only remove
`neuralInit` plus `featureArm` - 2.10 s - leaving `firstOutput` and the attach.
**Decision taken 2026-09-14: acceptance becomes "under 3 s for a warm toggle", and
the first segment's preroll, encode and mux plus the original-to-neural handoff
become a separate work item rather than part of the helper.** The "under 2 s" half
of the original criterion was written before anything measured the phases, and is
unreachable by making the helper resident however well it is done.

**Accepted at 2.44-2.52 s, median 2.47 s, over four driven player sessions, every
one `plan=reuse`** - against the under-3 s criterion, and against 5.23-5.41 s for
the first toggle in the same process. The reused job never incurs the 2.19 s of
process start-up, and it also shortened `firstOutput` from 1.437 s to 1.058 s on
the same clip in the same run, which the ~2.9 s floor estimate had assumed fixed;
so that floor was right about structure and wrong to treat the phase as fixed.
Which property of reuse shortens it - a kept NGX feature skipping a first-evaluate
warm-up, a decoder and encoder already up, or the rewound playhead - is
unmeasured. The shipped design, and all four of the lease, idle-VRAM, orphan and
identity problems residency had to answer, are in
[Architecture](ARCHITECTURE.md); the measured phases, the helper-side figures and
the warning not to read them as toggle-to-picture are in the
[player-session record](VERIFICATION-matrix.md).

**Two limits.** Residency is reached only when the second job's range is not
already covered by the first one's published entry - a toggle inside that coverage
is answered from the cache in about 0.8 s with no helper job, which is correct
behaviour and not this measurement. And per this document's own gate, **Blackwell
is still owed**: everything above is one Ada card on driver 610.47.

## What “2× / 3×” can mean

| Feature | Possible? | Meaning |
|---|---:|---|
| **2–3 Neural Rendering passes** | Yes | Feed each pass's output into another NR evaluation |
| **2–3× resolution** | Yes | DLSS Super Resolution enlarges the frame |
| **2–3× FPS** | Experimental | Frame Generation inserts new frames |
| **Intensity = 2.0** | Yes | Stronger model parameter—not two passes |

## P0 — Build first

> **Status.** Items 1-7 are implemented in this tree. See
> [Architecture](ARCHITECTURE.md), [Usage](USAGE.md) and the measured guide
> ablation in [Benchmark](BENCHMARK.md). Item 6 is the **open default**: opening a
> local file or a YouTube URL acquires and identifies the source, replays a
> validated cache entry when one exists, and otherwise plays the original, so
> rendering is always an explicit choice (frame, 4 s clip, marked range, whole
> video). Two residues are deliberate: item 5's "validated segment checkpoints"
> are bounded from-zero relaunches plus exact-frame retries, so mid-job resume
> that preserves temporal state remains future work; and since 2026-09-11 a render
> whose median neural GPU time falls below `NeuralGpuMsFloor` for its geometry is
> refused rather than published - the gap was a run reading
> `frames=900/900 verified=900` for DLAA-only output - while no samples at all is
> still accepted, because a missing measurement is not a verdict.

### 1. Runtime preflight and exact version locking

Before every render, run a short Feature 18 probe and record:

- GPU and driver
- DLL/runtime hashes
- Consumer and ReShade versions
- Every Feature 18 creation result
- Effective settings and output receipt

Do not automatically replace the proven RenoDX stack because a newer community build exists.

**Links:** [RuntimePolicy.cpp](../src/RuntimePolicy.cpp) · [runtime-lock.json](../packaging/runtime-lock.json) · [NVIDIA releases](https://github.com/NVIDIA/DLSS/releases) · [DLSS5 Bridge releases](https://github.com/NIGos/dlss5-bridge/releases) · [DLSS5 Feeder releases](https://github.com/jlrouzies-fr/DLSS5-Feeder/releases)

> Current community status: Bridge **1.4.12** is stable; 1.4.13 builds are prereleases. Feeder's newest listed build is **0.14.0-beta.5**.

### 2. Strong automated quality benchmark

Create a repeatable test set covering:

- Front-facing and profile faces
- Skin and hair
- Small text and subtitles
- Fine moving detail
- Highlights and gradients
- Camera cuts and fast motion

Measure neural time, total FPS, VRAM, temporal flicker, OCR accuracy, face consistency, color shift and deterministic rerenders. Include blind **one-pass versus two-pass** comparisons.

**Links:** [Benchmark script](measurements/runtime-comparison-20260907/benchmark.py) · [Face comparison](measurements/runtime-comparison-20260907/face-comparison.png) · [NVIDIA DLSS 5 research](https://research.nvidia.com/labs/adlr/DLSS5/)

### 3. Prove every guide and control

Ablate these independently:

- Motion vectors
- Depth
- RenoDX automatic mask (the custom mask guide was ablated, proven inert and removed)
- Structure intensity
- Tone intensity
- Render preset and style

Use identical warm-up and history conditions. Do not add expensive guide models until output differences prove they help.

**Links:** [DLSSBackend.cpp](../src/DLSSBackend.cpp) · [TemporalGuides.cpp](../src/TemporalGuides.cpp) · [HECer ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5)

### 4. Correct frame identity and temporal resets

Attach these identifiers to every frame, guide and neural result:

- Frame number
- Presentation timestamp
- History-generation ID
- Source/job ID

Reject mismatches. Reset every neural history after cuts, seeks, retries, source changes and worker restarts.

**Links:** [SynchronizedPlayback.cpp](../src/SynchronizedPlayback.cpp) · [TemporalGuides.cpp](../src/TemporalGuides.cpp) · [Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor)

> Scene-cut false positives (found 2026-09-10): the cut detector was stateless,
> so a single transition fired it repeatedly — 6 fires in 12 frames
> (#19119-19130) and 11 in 15 frames on a second clip. Every fire zeroes the
> motion vectors, drops the depth-proxy EMA, bumps the history generation and
> reaches NGX as `NVSDK_NGX_Parameter_Reset=1`, i.e. it throws away the
> accumulated history the reconstruction is built on; the DLSS Programming
> Guide 310.6.0 S3.13 asks for a reset only on the first frame after a major
> transition and notes that "improper use of this can result in temporal
> flickering, heavy aliasing or other visual artifacts". Shipped: the decision
> is split by strength, and the weak arm (moderate residual plus collapsed luma
> histogram) is now suppressed unless 0.6 s has passed since the last accepted
> cut — PySceneDetect's `min_scene_len` default, applied as the same hard
> minimum-interval filter. A strong residual still cuts immediately, which is
> how x264/x265 treat a decisive scenecut inside `min-keyint`. Every frame now
> carries its own verdict (strength, suppressed flag, residual, histogram
> overlap) so a live-playback decision is no longer invisible. Still open: the
> 0.30/0.10/0.85 thresholds are corpus-tuned, not validated against labelled
> cuts, and the accepted/suppressed counts do not yet appear in the render
> receipt next to the neural timings.

### 5. Stall recovery and resumable rendering

Implement explicit states for:

- Temporary GPU stall
- Worker crash
- Device removal
- Retry exhaustion
- User pause/resume

Use exact-frame retries, validated segment checkpoints and bounded recovery. Never silently omit a failed frame from final output.

**Links:** [NeuralWorker.cpp](../src/NeuralWorker.cpp) · [NeuralCache.cpp](../src/NeuralCache.cpp) · [DLSS5 Bridge stall/resume work](https://github.com/NIGos/dlss5-bridge/releases)

### 6. Range selection and preview-first workflow

Add:

- In/Out markers
- Exact timecode entry
- One-frame preview
- 3–5 second preview
- Required temporal pre-roll
- Accurate audio and variable-frame-rate handling

**Links:** [OfflineNeuralRenderer.cpp](../src/OfflineNeuralRenderer.cpp) · [VideoDecoder.cpp](../src/VideoDecoder.cpp) · [FFmpeg seeking documentation](https://ffmpeg.org/ffmpeg-all.html#Main-options) · [Visual Enhancer v7](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v7.0)

### 7. Clear strength and comparison controls

Provide:

- Original/Neural blend
- Freeze-frame comparison
- Split view and wipe
- Face/text zoom
- Separate Structure, Tone, Detail and Color controls
- Saved effective settings

The visual blend can be instant, but changing native model parameters may require rerendering.

**Measured 2026-09-14: keep all eight art defaults; one candidate for change.**
204 renders over four labelled clips, every group deterministic. Of the nine
alternatives the harness can express at the shipped mask state, three are
bit-identical to it (`NRPreset` 1/2/3), one is the carrier floor rather than an
art option (`NRIntensity=0`), four are worse or a wash (`NRAutoMask=0`,
`NRLocalStructure=0`, both `NRStyle` values), and exactly one points anywhere:
`NRLocalTone`. At 0.5 it recovers 2.0-3.9 dB PSNR and 2.6-5.7 delta-E on all four
clips, monotone, with SSIM within 0.005 and both temporal metrics within 0.31
levels - so the tone term moves colour and buys nothing measurable in stability.
It is NOT changed on that evidence: on fractals "closer to the source" is the only
thing PSNR and delta-E can mean, while on graded footage a deliberate relight is
the product. The decision was gated on the real-footage clips and on a filled
`blind.py` ballot, and **all three ballots have now been scored (2026-09-15): the
knob stays at 1.0.** Round 1 (1.0 against 0.5) went 6-2 to the candidate against a
pre-registered bar of 7; round 2 (1.0 against 0.0) went 8-1 with three ties, which
is P = 0.0195 on decided pairs but short of its literal bar of 10; round 3, cut on
two purpose-built 10-11 s continuous-motion clips with stills a full second past the
opening cut, **reversed** - five ties and four of the five decided pairs to the
shipped default. The motion subgroup that prompted round 3 went from 10 of 10 to 1
of 5 once the clips were long enough to carry temporal history, so it is retired as
noise rather than confirmed. Five decided pairs cannot clear 2 % in either
direction, so round 3 proves nothing on its own - what it does is fail the
replication test that the earlier pattern needed to survive. No round met its bar,
and the default stands on that rather than on a technicality.
[Report](measurements/art-defaults-20260914/REPORT.md) ·
[Ballots](measurements/tone-ballot-20260915/README.md)

Two defects that measurement found, neither fixed:
`NRPreset` is inert here - 0/1/2/3 are bit-identical on four synthetic clips at
both mask states and on one real graded film clip at the shipped mask state, two
repeats each, one output digest across all eight runs - yet it enters the render
identity through `CanonicalNeuralSettings`, so flipping it costs a full re-render
for byte-identical output. The knob was confirmed to reach the runtime rather than
be silently dropped: RenoDX echoes `preset=1|2|3` back in its own preflight
`activeSettings`, so the hint was written, read and ignored. Since the resident
helper landed the cost is doubled: a changed settings digest also evicts a healthy
helper, paying a cold bring-up of 2715-2751 ms against a warm 492-538 ms.

Left in deliberately. A zero-difference result earns only what it measured, so
this stays a property of DLSS-NR 310.8.0 with RenoDX 4.7 on Ada at driver
32.0.16.1047, not a property of the preset hint; the identity also carries the
runtime digest, so a future runtime that makes the knob live re-renders anyway.
Anyone who does narrow the key must narrow the helper's own
`SnapshotNeuralAddonSettings` comparison in the same change: the helper re-reads
its INI per job and fails the job outright on any textual difference, so a key
that treated two INI texts as equivalent while that comparison did not would turn
a cheap relaunch into a failed job plus a relaunch. Both read the same
canonicalisation today, which is where the single definition belongs.

`blind.py`'s candidate-frame filter used to admit nothing on clips with hard cuts,
fall back to frame 0, and hand both pairs of `cuts-motion` and `cuts-similar` the
same frame - which made the one instrument that could settle the tone question
useless on half the corpus. **Fixed 2026-09-14.** Shots are now derived from the
manifest's own `cuts` and `soft_cuts`, a candidate must sit past a 0.1 s guard
after the cut that opened its shot and leave at least 0.5 s of that shot behind
it, and the excerpt is clipped to the shot so it never spans an edit - `--seconds`
is a cap, not a length. There is no frame-0 fallback left: a shot too short to
serve is named in `key.json` as `dropped_shots`, a clip with no usable shot as
`skipped_clips`, and a run with nothing left to judge fails with the reason. The
roadmap's own complaint retested with its flags now picks `cuts-motion` frames 5
and 59 and `cuts-similar` 11 and 33, in four different shots. The tone ballot is
therefore runnable, and it was run three times and scored - the record is in
[the ballots](measurements/tone-ballot-20260915/README.md). Using it on a real
question found four more defects in it: a rebuild left the previous ballot's images
in the directory the judge is told to look at, an unfilled ballot scored as a clean
sweep of zeros and exited 0, a blank confidence column became a weighting of 1.0 that
printed as though measured, and frames were drawn with replacement so a single-shot
clip could hand the judge the same comparison twice. All four are fixed; the guard
and the frame separation are now per-ballot parameters recorded in `key.json`.

**Links:** [ReShadeConfig.cpp](../src/ReShadeConfig.cpp) · [D3D12Renderer.cpp](../src/D3D12Renderer.cpp) · [video2dlssnr controls](https://github.com/DaniilSokolyuk/video2dlssnr)

## P1 — High-value quality and performance

> **Status (September 9, 2026).** Item 9 is implemented: acceptance by evidence
> margin, a banded reverse check and a confidence-weighted vector median cut
> false motion on the cuts clip from 60.8 % to 3.7 % and made the guide pass
> cheaper (6.66 → 3.09 ms). Item 10 was measured and abandoned on the NGX path —
> the mask is inert for neural rendering and for DLSS-SR alike, and two of its
> three parameter names belong to Ray Reconstruction ([Benchmark](BENCHMARK.md));
> compositing outside NGX remains open. Item 12's render-ahead half shipped as
> the active session, and its premise changed: the capture no longer swizzles on
> the CPU, and the residual readback is the proportional term of the measured
> 7.35 ms + 2.50 ms/megapixel cost rather than a dominant one. Item 13 was
> measured and rejected (two-pass costs 2.8 dB PSNR and 11 OCR points). Items 8,
> 11 and 14–18 are open; item 15's own precondition has now half-fired, since
> depth measurably changes the output by less than 0.02 dB.

### 8. Source-color and HDR preservation

Preserve:

- Primaries and transfer functions
- Full/limited range
- 10-bit precision
- HDR metadata
- Original chroma when requested

Expose native **Tone Intensity**, including zero, which NVIDIA says preserves the rendered frame's exact colors. Add clipping and color-shift warnings.

**The source's own colour description is now read, and it gates the GPU
conversion (2026-09-15).** `VideoDecoder`'s existing ffprobe call also asks for
`color_space`, `color_range`, `color_primaries` and `color_transfer`, and the
description travels to the renderer, which compiles the NV12 source pass for
exactly the pair the source declared - BT.709 or BT.601, limited or full. A source
declaring anything else, or nothing, decodes to BGRA and ffmpeg converts it on the
CPU: undeclared is not treated as BT.709. Every render's log answers
`GPU source conversion` with `accepted:`/`refused:` and the four tags it read.
What that was worth, measured on a JPEG (`pc`/`bt470bg` - BT.601 full by
convention): decoding its NV12 with the old hard-coded BT.709-limited
coefficients lands mean 5.89 and max 33.0 eight-bit levels from its true RGB,
against 0.40 and max 2.0 for the program the probe now selects. That was only
reachable with `GpuSourceConversion=1`, which is exactly why the flag was off -
the hazard is now removed rather than documented, and the flag's default is a
throughput question again. Primaries and transfer are read and logged but do not
select a conversion: the pass produces R'G'B' from Y'CbCr, a matrix and a range
and nothing else, and the CPU fallback handles a BT.2020-primaries or PQ source no
better. 10-bit, HDR metadata and original-chroma preservation remain open.

**Unmeasured, and deprioritised on 2026-09-14 - not a negative result.** The
exposure half of this item - supplying NGX a 1x1 exposure texture and an `IsHDR`
flag instead of the `AutoExposure` feature flag `DLSSBackend.cpp` sets
unconditionally - was listed as runnable once the flag reached a harness profile,
and it still is. Nothing has measured it: that day's sweep varied the RenoDX `NR*`
keys and never touched the NGX flag or an exposure texture, so it says nothing
either way about this item, and an earlier draft of this note wrongly implied it
did. The reason for deferring is ordering, not evidence - plumbing a flag to
measure an untested suspicion while the persistent helper had a measured 2.10 s on
the table was the wrong order of work.

What would warrant it, specifically: an HDR10 or PQ source, which the corpus does
not have and cannot synthesise honestly, or a clip where auto-exposure demonstrably
misreads - `highlights-gradients` clips to white by construction and is the
candidate to check first. Either gives the A/B something to be about.

**Links:** [NVIDIA DLSS 5 controls](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [neural-upstream](https://github.com/matiasLombo/neural-upstream) · [MediaPipeline.cpp](../src/MediaPipeline.cpp)

### 9. Confidence-aware optical flow

**NVOFA is in.** Motion comes from the engine on a 2x2 grid in S10.5, with the
CPU estimator kept as the fallback for cards and builds without it. What that
leaves open is the confidence half: the engine's cost surface is produced and
bound, but the gate is off because its thresholds have not been measured, and
nothing yet uses forward/backward disagreement.

**And NVOFA now reaches playback-SR sessions too (2026-09-14).** It used to run
only when the decoded frame already matched the DLSS input size, so turning
runtime Super Resolution on silently dropped motion estimation to the CPU
block matcher - a perf and quality cliff exactly where the user asked for more
quality. The engine is now given the decoded frame's geometry and the resolve
pass scales the vectors per axis into the DLSS input grid, which is exact because
the convert pass is one full-screen triangle over uv 0..1 with no crop. The
neural-size path is byte-unchanged: the scale is `1,1` there, asserted with an
exact float compare rather than an epsilon, and a live session prints
`Motion guide backend: NVOFA hardware flow on the decoded 1920x1080 frame,
vectors scaled by 1,1 into the 1920x1080 DLSS input`. The SR branch itself is
unit-tested and **now session-proven** (2026-09-14 said otherwise and was wrong):
two real sessions created and evaluated a SuperResolution feature at
`input=1920x1080 output=2560x1440` for 600+ frames with NVOFA hardware flow as the
motion guide in that renderer. The `NGXLoadLibrary: 126` line read as a blocker is
NGX's app-local probe for the NGX *core*, which no application ships; the same log
resolves it from the driver store four lines later and says `succeeded`. The player's
log showed only the failure because its NGX callback forwards messages containing
"error" and nothing else. What the ratio cannot be is anything but `1,1`: the SR path
passes `preserveSource=true`, so the DLSS input *is* the decoded frame at all six
renderer call sites, so the "scale other than 1,1" this item once wanted as evidence
was unobtainable by construction - see
[the SR session report](measurements/p5-sr-session-20260915/REPORT.md).

**One thing the SR session exposed, and it is owed.** The numeric side of the
per-axis plan is well covered - `RenderSettingsTests.cpp` drives identity,
downscaled, 1.5x upscaled, anisotropic and zero-dimension refusal - but the *GPU
consumer* of that number is not: `motion*MotionScale` in `src/NvofResolveShader.h:32,72`,
fed by the constants at `src/D3D12Renderer.cpp:828-830`, has compile-only coverage
(`tests/UpscalingTests.cpp:60-72` compiles the shader and asserts nothing about its
arithmetic). So the shader multiply by a *non-unit* scale has never executed: not in
a test, because no test runs that pixel shader, and not in a session, because every
live configuration supplies `1,1`. The factor is tested; the multiply that consumes
it is exercised only at the identity, where it is a no-op. No test is added here for
a configuration the player cannot currently produce - that would pin unreachable
code - but the risk is named: it becomes real with the first configuration that
resamples the decoded frame into a differently sized DLSS input, and the way to
retire it is a GPU test driving `RenderFrame` with `renderW != sourceW` that checks
the emitted motion texture.

The 2026-09-11 survey found the cheap route to both, and it is items 1 and 2 of
the plan at the top of this file: `NV_OF_PRED_DIRECTION_BOTH` returns backward
flow and its cost from the same Execute, which makes the disagreement test a
measurement in pixels rather than a guess at a cost threshold, and
`enableGlobalFlow` returns the per-frame global vector the cut detector is
missing. Neither needs a second session or a second Execute. NVIDIA publishes no
numeric meaning for the cost values in any document, so calibrating the cheap
gate against the forward/backward mask is the only honest way to keep it.

Still worth comparing the engine against RAFT and a Lucas-Kanade fallback on
the corpus before assuming it wins everywhere.

**Links:** [NVIDIA Optical Flow SDK](https://github.com/NVIDIA/NVIDIAOpticalFlowSDK) · [video2dlssnr NVOFA pipeline](https://github.com/DaniilSokolyuk/video2dlssnr) · [HECer ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5)

### 10. Stable protection masks

**Measured September 8, 2026: the NGX mask inputs are inert.** A mask with
5–24 % non-zero cells and a forced all-ones mask both produced pixel-identical
output on feature 18, preset K and preset Default alike, and mask on versus off
is 0 differing bytes on the DLSS-SR path while motion vectors change 6.85 % of
bytes. `DLSS_Input_Bias_Current_Color_Mask`, `DLSS_DisocclusionMask` and
`DLSS_ResponsivityMask` are Ray Reconstruction inputs. The mask guide was
deleted; any future protection work must composite outside NGX rather than bind
a mask to it.

Support temporally tracked masks for:

- Faces and skin
- People
- Text and subtitles
- Imported user masks
- Regions that must remain unchanged

Feather masks and preferably composite subtitles after neural processing.

**Links:** [MediaPipe Face Landmarker](https://developers.google.com/mediapipe/solutions/vision/face_landmarker) · [NVIDIA semantic/engine masks](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/) · [TemporalGuides.cpp](../src/TemporalGuides.cpp)

### 11. RTX Video enhancement modes

Expose these as separate operations:

1. **Artifact Reduction** — deblocking/banding cleanup without resizing
2. **Video Super Resolution** — spatial upscale
3. **RTX Video HDR** — SDR-to-HDR conversion

Benchmark:

- Artifact Reduction → Neural Rendering → SR
- Neural Rendering → VSR
- NR only
- RTX Video only

**Links:** [RTX Video SDK 1.1](https://developer.nvidia.com/rtx-video-sdk/getting-started) · [Visual Enhancer v7](https://github.com/Merserk/dlss5-visual-enhancer/releases/tag/v7.0)

### 12. GPU-resident processing and buffered viewing

Reduce CPU readbacks and use bounded GPU queues for decode, neural processing and encode.

Offer two distinct modes:

- **Render-ahead playback:** watch completed segments
- **Experimental live mode:** bounded latency with visible dropped-frame counters

Dropped frames may be acceptable for live preview, never final export.

**Audited end to end 2026-09-14, and the result is mostly a refusal.** Fourteen
CPU round-trips on the live segment-capture path were enumerated with file:line
and each marked necessary or removable. Two removals landed - five
`ClearRenderTargetView` calls that wrote memory the very next full-target draw
overwrote (66 MB + 33 MB + up to 33 MB per frame of write bandwidth at 4K), and a
per-frame Map/Unmap of the timestamp readback, now persistently mapped. Neither
is measurable: 1080p median moved -0.8 % and 4K +0.4 % against a session spread
three to five times larger, so **item 12's own success criterion is unmet** and
they are kept as bandwidth hygiene with a correctness argument, not as a win.

The largest proportional item turned out not to be code at all but two policy
defaults, `[Encoding] GpuSourceConversion` and `GpuColorConversion`, both off,
which put 4 B/px instead of 1.5 across the decoder pipe and the capture readback.
Turning both on is worth **+7 % processing throughput at 4K** and costs **0.75 dB
PSNR, +0.95 dE and double the false motion**, and single-flag arms attribute it:
the capture side alone costs -0.79 dB / +0.94 dE, the decoder side alone -0.64 dB
/ +0.88 dE on an **untagged** clip. Re-run on a `bt709`-tagged one, the two split:
the decoder-side flag becomes **free** (+0.067 dB, +0.009 dE) while the
capture-side flag still costs **-0.64 dB / +0.64 dE**. The source is already
`yuv420p`, so nothing lost chroma resolution; what happened is that both shaders
hard-code BT.709 limited range (`src/D3D12Renderer.cpp:316-341`) while swscale
falls back to BT.601 for a stream that declares nothing. So `GpuSourceConversion`
is a **tagging defect away from free** - exactly the hazard
`docs/USAGE.md:219-221` already names - and its blocker is the source colour-tag
probe, not readback cost. `GpuColorConversion`'s cost survived tagged *input* at
both resolutions, so it was not an input-tagging artifact - it turned out to be an
output-side colour-metadata defect instead, and it is now gone. See below.

**The same audit walked into a larger defect, and fixing it was the wave's most
user-visible change.** Every render written on the default path was **untagged and
BT.601-converted**: the encoder stated colorimetry only when the GPU converted the
frame, so a `bt709` source became a file that declared nothing - verified on an
ordinary render, and the matrix measured directly (a pure-red frame returns the
BT.601 prediction at 1080p and 480p alike, so swscale does not switch on
resolution). Both paths now state `bt709`/`tv` and the CPU path converts with
`out_color_matrix=bt709`; labelling without converting would have been worse than
the ambiguity. PSNR never saw any of it, because each path round-trips under its own
tags, which is worth remembering the next time a colour question is handed to a
fidelity metric. All four tags land now, on both paths, via `setparams` -
the `-color_*` output options carry only matrix and range on this FFmpeg. Stamping
them on the NV12 path's frames also **removed the capture-side quality cost
entirely** (0.00 dB against the CPU path, where it had been -0.64 dB), which retired
the chroma-siting hypothesis before it was tested: the cost was frames reaching the
encoder undescribed, not sample positions. `GpuColorConversion` is therefore a
candidate for defaulting on - no quality cost, +5.8 % throughput at 4K, and on this
card it lowers GPU ms where the shipped note measured the opposite on a 5070 Ti, so
re-measure that before flipping. `GpuSourceConversion` still needs the source
colour-tag probe, which the export fix does not provide. The defaults stay this
wave, the
flags remain available per render, and the measurement is the reason rather than
the taste: [readback report](measurements/gpu-readback-20260914/REPORT.md).

**Links:** [MediaPipeline.cpp](../src/MediaPipeline.cpp) · [D3D12Renderer.cpp](../src/D3D12Renderer.cpp) · [NVIDIA Video SDK samples](https://github.com/NVIDIA/video-sdk-samples) · [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr)

## P2 — Experiments that must prove value

### 13. Two-pass and three-pass Neural Rendering

Yes, this is possible on video.

Recommended implementation:

```text
Source + guides
  → NR pass 1, independent history
  → NR pass 2, independent history
  → optional NR pass 3, independent history
  → one guarded composition against the original
```

Rules:

- **1 pass:** default
- **2 passes:** experimental preset
- **3 passes:** advanced option
- **More than 3:** research only
- Reduce strength on later passes
- Reset every pass's history together
- Include pass count and per-pass settings in cache keys

Community experiments show extra structure from two passes, but also stronger brightness/color shifts, halos and diminishing returns.

**Links:** [Pre-SR Multipass, 1–3 passes](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) · [THERMOTRON Multipass, 1–4 passes](https://github.com/edgarbatjr/OptiScaler_DLSSNR-THERMOTRON-multipass) · [Two-pass live video demonstration](https://github.com/jpneagle/dlss5-webcam-demo)

### 14. Neural-before-upscale and reduced working resolution

Test this full matrix:

- NR at native resolution
- SR → NR
- Lower-resolution NR → upscale
- Low-resolution neural residual transferred to the original
- Working scale: 50%, 75%, 100%
- Passes: 1, 2, 3

Judge faces, text, shimmer, highlights, speed and VRAM separately.

**Links:** [neural-upstream](https://github.com/matiasLombo/neural-upstream) · [Pre-SR Multipass](https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass) · [OptiScaler DLSS-NR](https://github.com/Dagherbou/OptiScaler_DLSSNR)

### 15. Learned depth and normals

Only implement after guide ablations prove depth affects the active consumer.

Suggested order:

1. Video Depth Anything Small FP16
2. FlashDepth for lower-latency experiments
3. Derived normals
4. DSINE only if normals demonstrably matter

These are estimated guides, not ground-truth scene geometry.

**Links:** [Video Depth Anything](https://github.com/DepthAnything/Video-Depth-Anything) · [FlashDepth](https://github.com/Eyeline-Labs/FlashDepth) · [DSINE](https://github.com/baegwangbin/DSINE) · [HECer depth backend](https://github.com/HECer/ComfyUI-DLSS5)

## P3 — Later expansion

### 16. Frame Generation: 2× and 3× FPS

Treat Frame Generation as a separate pipeline from Neural Rendering.

Validate:

- Scene cuts
- First and last frames
- Generated-frame timestamps
- Audio duration and synchronization
- Long sequences
- One versus two inserted frames

HECer's worker currently provides an experimental **2× smoke test**, not a complete quality validation. A 3× mode requires producing two correctly timed intermediate frames.

**Links:** [DLSSG Stream Worker](https://github.com/HECer/DLSSG-Stream-Worker) · [Worker protocol](https://github.com/HECer/DLSSG-Stream-Worker/blob/main/docs/PROTOCOL.md) · [ComfyUI DLSS Frame Interpolation](https://github.com/Konohamaru04/ComfyUI-NVIDIA-DLSS-Frame-Interpolation)

### 17. Editor, CLI and ComfyUI interoperability

Study or optionally support compatible workflows rather than rebuilding every interface immediately.

**Relevant projects:**

- [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr) — D3D12 CLI/UI/ComfyUI pipeline with NVOFA, scene resets, 10-bit output and color tagging
- [DaVinci Resolve DLSS5](https://github.com/SAOG0721/DaVinci-Resolve-DLSS5) — experimental OpenFX filter
- [ComfyUI-DLSS5-Enhancer](https://github.com/Blueforcer/ComfyUI-DLSS5-Enhancer) — image/video NR nodes
- [DLSS5 Visual Enhancer](https://github.com/Merserk/dlss5-visual-enhancer) — NR, Frame Interpolation and RTX Video workflows
- [HECer ComfyUI-DLSS5](https://github.com/HECer/ComfyUI-DLSS5) — persistent processing, guides and depth

### 18. Alternative adapters, multi-GPU and cross-platform research

Keep outside the critical path until single-GPU processing is reliable.

- Evaluate only one neural consumer at a time
- Isolate RenoDX, Deep Fried Chicken and ShortFuse adapters
- Add explicit dual-consumer detection
- Consider multi-GPU transport later
- Treat model extraction and redistribution as a licensing boundary

**Links:** [DLSS5 Feeder](https://github.com/jlrouzies-fr/DLSS5-Feeder) · [DLSS5 Bridge](https://github.com/NIGos/dlss5-bridge) · [Neural Coprocessor](https://github.com/maohgad-web/Neural-coprocessor) · [MLX-DLSS research](https://github.com/iamwavecut/MLX-DLSS) · [OptiScaler DLSS-NR](https://github.com/Dagherbou/OptiScaler_DLSSNR)

## Recommended implementation order

**1–7 reliability → 8–12 quality/performance → 13 multipass → 14 processing order → 15 learned guides → 16 Frame Generation → 17–18 ecosystem expansion**

The highest-value additions are **runtime preflight, stall recovery, preview-first rendering, source-color protection, RTX artifact reduction, and a carefully isolated two-pass backend**.
