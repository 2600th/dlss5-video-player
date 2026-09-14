# GPU-resident path (P4) and NVOFA for SR sessions (P5) - 14 September 2026, RTX 4080 SUPER

**Verdict: P4's own success criterion is NOT met by the two changes that landed, and
the one change that would meet it must not be made, because it costs measurable image
quality.** The five redundant `ClearRenderTargetView` calls and the per-frame timestamp
Map/Unmap are gone and are bit-exact, but neither is measurable on this machine: the
1080p live-session median moves -0.094 ms/frame (-0.8 %) and the 4K offline median moves
+0.078 ms/frame (+0.4 %, i.e. the wrong way), against a session-to-session spread of
0.22 ms at 1080p and 0.28-0.44 ms at 4K. The audit's largest proportional item is not a
code path at all but two policy defaults - `gpuSourceConversion` and `gpuColorConversion`,
both FALSE - and flipping them buys +7.0 % throughput for -0.75 dB PSNR, +0.95 dE and a
doubling of false motion (0.0013 -> 0.0026). **Do not flip the defaults.** P4's stated
criterion, "measurable drop in ms/frame or keep-up forecast margin on a known 4K clip",
is therefore recorded as UNMET: the cheap changes are below noise, and the change that
would move the number is a fidelity regression.

P5 landed and is proven on the neural-size path by a live session log line, with the
identity scale that shows the fast path is byte-unchanged. It is **not** proven in an
actual Super Resolution session on this machine, and that evidence is owed; see the P5
section for the exact reason and the exact run that would produce it.

## Environment and instruments

| Item | Value |
|---|---|
| GPU / driver | NVIDIA GeForce RTX 4080 SUPER, driver `32.0.16.1047` (610.47) |
| CPU / OS | Intel Core i7-14700, Windows 11 Pro 10.0.26200 |
| Runtime | DLSS-NR 310.8.0, ReShade 6.8.0.2155, RenoDX 4.7 |
| Pre-change tree | `eab9bc2`, built in a separate worktree |
| Post-change tree | this working tree |
| Offline renders | `tools/benchmark/run.py` (the real offline worker), `--repeats 3` |
| Scoring | `tools/benchmark/analyze.py` against the lossless source |
| Live sessions | `tools/verification/player_session.ps1 -Sessions 3` (a real GUI player session) |
| Profiles | [`nv12.profile.json`](nv12.profile.json) in this directory (`cpu-conversion`, `gpu-conversion`) |

Renders are bit-identical across repeats within an arm, verified again today by
decoded-frame sequence digest. Every timing number below is a median of three; every
quality number is a single scored run per arm, which is sufficient precisely because the
repeats are bit-identical (digests in the quality section).

## What landed in code

**1. Five redundant `ClearRenderTargetView` calls removed.** `src/D3D12Renderer.cpp`
around :498-503 (`m_dlssColor`, render-size FP16, 8 B/px), :858 (convert pass), :922
(backbuffer), :1051-1055 (cacheOutput/luma), :1084 (chroma), :1200 (static present). Each
was a full-target write immediately before a full-screen triangle over a viewport equal to
the whole target, blending off, write mask ALL, so the clear could only write memory the
draw then overwrote. At 4K that is 66 MB (dlssColor FP16) + 33 MB (backbuffer) + 33 MB
BGRA or 12.4 MB NV12 of write bandwidth per frame. `m_dlssColor` also lost an optimized
clear value with no remaining consumer. The depth pass KEEPS `ClearDepthStencilView`
(`src/D3D12Renderer.cpp:844`), because a depth clear also resets hierarchical-Z state and
NGX reads that resource. `ClearRenderTargetView` no longer appears anywhere in
`src/D3D12Renderer.cpp`.

**2. The timestamp readback is now persistently mapped.** `src/D3D12Renderer.cpp:666-677`
maps it once at resource creation, `:1245` reads through the mapped pointer, `:142-143`
unmaps at teardown, replacing a Map/Unmap pair per frame in `HarvestNeuralTimings`
(`:1235-1248`), which both `RenderFrameInternal` and `PresentCurrent` call. This mirrors
the capture readbacks' existing persistent mapping and its coherence argument: the buffer
is written by `ResolveQueryData` and read only after the fence that published it.

## P4 measurement 1: the two landed changes are below noise

Same machine, same media, same harness; pre-change tree (`eab9bc2`, built in a separate
worktree) against the post-change tree.

1080p live sessions, `player_session.ps1 -Sessions 3`, the reported
`Measured neural render pace` in ms/frame, 90 s clip, 1560-frame window:

| tree | session 1 | session 2 | session 3 | median |
|---|---:|---:|---:|---:|
| pre-change `eab9bc2` | 11.7177 | 11.6291 | 11.8529 | 11.7177 |
| post-change | 11.6438 | 11.6239 | 11.4616 | 11.6239 |

4K (3840x2160), deterministic offline route, `run.py --repeats 3`, `gpu_ms_p50`:

| tree | repeat 1 | repeat 2 | repeat 3 | median |
|---|---:|---:|---:|---:|
| pre-change `eab9bc2` | 18.224128 | 18.079744 | 17.947648 | 18.079744 |
| post-change | 17.734656 | 18.179072 | 18.157568 | 18.157568 |

The 1080p median moves **-0.094 ms/frame (-0.8 %)** and the 4K median moves **+0.078
ms/frame (+0.4 %)**, which is the wrong way, against a session-to-session spread of 0.22 ms
at 1080p and 0.28-0.44 ms at 4K. The honest reading: the removals are not measurable on
this machine at either resolution, the spread swamps them, and the +0.4 % at 4K is not
evidence of a regression either - the post-change arm contains both the fastest (17.734656)
and the second-slowest (18.179072) 4K repeat of the six.

**These two changes are kept as bandwidth hygiene with a correctness argument, not as an
optimization with a measurement behind it.** A clear that writes memory the next draw
fully overwrites is waste whether or not this machine's fast-clear path makes the waste
visible, and a Map/Unmap pair per frame around a four-value read is waste on the same
footing. Nothing in this section may be cited as an improvement: **no measurable
improvement was observed.**

**P4's stated success criterion - "measurable drop in ms/frame or keep-up forecast margin
on a known 4K clip" - is UNMET.**

### The obstacle to live 4K measurement, named rather than worked around

The 4K arm had to use the offline route because the live 4K session is blocked in BOTH
trees, identically: `player_session.ps1` reports
`a modal dialog blocked the session: #32770|Neural rendering from here`, exit 9, on all 3
sessions of each tree. Since it reproduces identically on `eab9bc2` and on the post-change
tree, it is an interaction, not a regression of this work - but it is a real obstacle to
live 4K measurement, and the keep-up forecast margin half of P4's criterion cannot be
evaluated at 4K at all until it is cleared. The 1080p live arm above is the only live
evidence in this record.

### The 4K clip

`build-upscaling/benchmark-corpus-4k/demo-4k-60.mkv`, 60 frames, digest prefix
`bbab86df491207c9`, cut from `docs/media/neural-comparison-demo.mp4` frames 120-179, video
surface cropped then lanczos to 3840x2160. It is a throwaway measurement corpus and is not
committed. That source is the player's own DLSS-NR output, screen-captured, so this clip is
an **NR-processed capture**, not camera-original footage. It is neither synthetic nor
publisher-released, and no number in this record pools it with anything else.

## The readback audit

Scope: the live segment-capture path, decode -> guides -> RenderFrame upload -> DLSS ->
capture draw -> readback -> `CopyCaptureView` -> ffmpeg pipe. Line numbers are post-edit.
Rows are reproduced from `GpuPath`'s audit; the kept `ClearDepthStencilView` is given its
own line rather than living inside the clear row's verdict, so that every call site in the
table has one row and one verdict.

| # | site | what crosses the CPU | verdict |
|---:|---|---|---|
| 1 | `src/VideoDecoder.cpp:813-814` + `src/OfflineNeuralRenderer.cpp:1656` | decoder delivers BGRA 4 B/px unless `preferNv12`; `preferNv12 = request.gpuSourceConversion`, DEFAULT FALSE (`src/OfflineNeuralRenderer.h:57`, `src/main.cpp:1481`, `m_gpuSourceConversion=false` at `main.cpp:4634`) | **REMOVABLE, NOT IMPLEMENTED - and now MEASURED: DO NOT FLIP.** The single largest item in the proportional term: with it off, ffmpeg converts NV12->BGRA on the CPU inside the decoder child and 4 B/px instead of 1.5 crosses the pipe and the upload heap. The GPU path is already built (`PSSourceNv12`, `src/D3D12Renderer.cpp:770-779`) and is one ini key away. Measurement 2 below prices it: +7.0 % throughput for -0.75 dB PSNR. Stays FALSE. |
| 2 | `src/D3D12Renderer.cpp:752-754` `CopyMappedRows` -> upload heap | one full frame of CPU writes per frame | NECESSARY. The decoder hands over host memory; something must write it into the upload heap. Already a single contiguous parallel memcpy when `RowPitch` equals the tight row, and 1.5 B/px instead of 4 the moment row 1 is on. |
| 3 | `src/D3D12Renderer.cpp:755` guide grid upload | 160x90 RGBA32F, ~230 KB | NECESSARY. Not proportional to frame area in any way that matters - it is the analysis grid, not the frame. |
| 4 | `src/D3D12Renderer.cpp:770-771`, `:782`, `:786` `CopyTextureRegion` | GPU-side copies, no CPU touch | NECESSARY. |
| 5 | `src/OpticalFlowNvof.cpp:449` + `:452-464` `ReadbackGlobalFlow` | 4 bytes/frame out of a 1x1 surface, fence-compared, never waited on | NECESSARY as written and negligible. Backlog note: nothing consumes `LastGlobalFlow()` yet. |
| 6 | `src/D3D12Renderer.cpp:1074` `CopyTextureRegion` capture plane -> readback buffer | THE proportional readback: 4 B/px BGRA, 1.5 B/px NV12 | **NECESSARY in kind; its SIZE is a policy - and that policy is now MEASURED: DO NOT FLIP.** The 2.7x is available today without a rebuild, since `request.gpuColorConversion` is also DEFAULT FALSE (`src/OfflineNeuralRenderer.h:49`, `src/main.cpp:1480`, `m_gpuColorConversion=false` at `main.cpp:4626`). Twin of row 1, and the row that carries the quality cost. Inferred mechanism: the NV12 readback would subsample chroma before the encoder sees it. |
| 7 | `src/D3D12Renderer.cpp:1112` `WaitForFenceValue(m_captureFence[slot])` | CPU stall on one slot | NECESSARY. Per-slot, not a drain; already counted as `m_captureResolveWaitNanos`, and the resolve-wait row of the stage table is what measures it. |
| 8 | `src/D3D12Renderer.cpp:1151-1170` `CopyCaptureView` (called off-thread at `src/OfflineNeuralRenderer.cpp:1751`) | one full frame of CPU memcpy per frame | NECESSARY, for two nameable reasons: (a) the encoder is an ffmpeg child fed tightly packed rows over a pipe and readback rows are `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT`-padded - NV12 luma at 1920 wide is pitch 2048 - so writing from the mapped buffer directly means one `WriteFile` per row; (b) it must not hold a readback slot across a blocking pipe write, or ffmpeg back-pressure would stall the capture ring. Already off the render thread and overlapped with decode/guide/submit. |
| 9 | `src/OfflineNeuralRenderer.cpp:2172` `encoder.WriteFrame` -> pipe | one full frame | NECESSARY; already on its own thread. |
| 10 | `src/D3D12Renderer.cpp:1235-1248` `HarvestNeuralTimings` | WAS a Map + Unmap of the timestamp readback on every frame (both `RenderFrameInternal` and `PresentCurrent` call it) | **REMOVABLE - IMPLEMENTED.** Persistently mapped at `:666-677`, read at `:1245`, unmapped at `:142-143`, mirroring the capture readbacks' existing persistent mapping and its coherence argument. Fixed-term, not per-pixel - and below noise, see measurement 1. |
| 11 | `src/D3D12Renderer.cpp:1254` `SampleLocalVideoMemory` / `QueryVideoMemoryInfo` per frame | one O(1) DXGI call per frame | NECESSARY as written. Constant cost, not in the per-pixel term, and the peak it feeds is a receipt field. Sampling it less often would only weaken the receipt. |
| 12 | five `ClearRenderTargetView` calls, now gone: `src/D3D12Renderer.cpp:498-503` (`m_dlssColor`, was render-size 8 B/px), `:858` (convert pass), `:922` (backbuffer), `:1051-1055` (cacheOutput/luma), `:1084` (chroma), `:1200` (static present) | a full-target write immediately before a draw that writes every texel of the same target | **REMOVABLE - IMPLEMENTED, bit-exact.** Every one of these passes is the same full-screen triangle over a viewport equal to the whole target, blending off, write mask ALL, so the clear could only ever write memory the draw then overwrote. At 4K that is 66 MB (dlssColor FP16) + 33 MB (backbuffer) + 33 MB BGRA or 12.4 MB NV12 (capture planes) of write bandwidth per frame. `m_dlssColor` also lost its optimized clear value, which no longer has a consumer. Below noise, see measurement 1. |
| 13 | `src/D3D12Renderer.cpp:844` `ClearDepthStencilView` (depth pass) | a full depth-target write before the depth-write draw | NECESSARY, and deliberately kept. A depth clear also resets hierarchical-Z state, and NGX reads that resource, so the argument that retires row 12 does not apply here. |
| 14 | `src/D3D12Renderer.cpp:576-584` `m_cacheOutput` | not a readback: a full output-size `B8G8R8A8` target allocated unconditionally and never written when the capture format is NV12 (`outputW*outputH*4` = 33 MiB at 4K) | **REMOVABLE, DELIBERATELY NOT IMPLEMENTED.** It is an allocation, not a CPU round-trip, and idle/peak VRAM is not this slice's subject. It belongs to whoever next touches VRAM. Only the guard at `:1035` and the RTV at `:584` would need to become format-aware. |

Three rows are REMOVABLE-IMPLEMENTED or REMOVABLE-NOT-IMPLEMENTED in a way that needed a
decision rather than a patch: rows 1 and 6 are the conversion defaults, priced below and
staying FALSE; row 14 stays not-implemented and is handed on.

## P4 measurement 2: the audit's biggest item, and why the default must stay

The two defaults in rows 1 and 6 are read from the player ini at `src/main.cpp:1480-1481`
(`[Encoding] GpuColorConversion` / `GpuSourceConversion`), defaulting to 0, and land in
`NeuralRenderRequest` at `src/OfflineNeuralRenderer.h:49` and `:57`, both `false`. With
them off, ffmpeg converts NV12 -> BGRA on the CPU inside the decoder child, so 4 B/px
instead of 1.5 crosses the pipe and the upload heap, and the capture readback is BGRA
4 B/px instead of NV12 1.5 B/px.

A `worker_flags` passthrough was added to `run.py` (profile-level; see
`tools/benchmark/run.py:48-55`, `:320-327`, and [`nv12.profile.json`](nv12.profile.json)),
so this is an A/B over two profiles rather than a rebuild. 4K, 3 repeats each:

| arm | wall s | processing fps | gpu_ms_p50 |
|---|---|---|---|
| cpu-conversion (shipped default) | 10.62 / 10.50 / 10.59 | 30.97 / 31.18 / 30.89 | 18.051072 / 18.020352 / 18.191360 |
| gpu-conversion (both flags on) | 9.88 / 9.88 / 10.08 | 33.26 / 33.35 / 33.64 | 18.355200 / 18.492416 / 18.270208 |

Throughput improves: median processing fps 31.18 -> 33.35, **+7.0 %**, computed from those
two medians; the wall median 10.59 -> 9.88 s is -6.7 %. GPU ms/frame rises 18.02 -> 18.36,
**+1.9 %**, which is the expected shape - work moves off the CPU and the pipe onto the GPU,
so the GPU does more per frame while the pipeline as a whole finishes sooner. (For
transparency about which statistic that pair is: 18.36 is the gpu-conversion median and
18.02 is the cpu-conversion arm's fastest repeat. Median against median it is
18.051072 -> 18.355200, +1.7 %. The sign and the magnitude do not depend on the choice.)

Quality, scored by `analyze.py` against the lossless source. Each arm is bit-identical
across its 3 repeats - cpu digest prefix `513fa743745e6946`, gpu `e67dd53e5f284b80` - so
one scored run per arm is sufficient and the deltas below are deterministic, not sampled:

| arm | PSNR dB | dE mean | flicker+ | sigma+ | false motion |
|---|---|---|---|---|---|
| cpu-conversion | 29.09 | 5.37 | 0.145 | 0.492 | 0.0013 |
| gpu-conversion | 28.34 | 6.32 | 0.147 | 0.479 | 0.0026 |

**That is the decision: -0.75 dB PSNR, +0.95 dE, and false motion doubled (0.0013 ->
0.0026) for +7 % throughput. Do not flip the defaults.**

The mechanism is not a transport change, it is a precision change. The BGRA readback
carries full-resolution chroma to the encoder, while the NV12 readback would subsample
chroma 4:2:0 before the encoder ever sees it. Mark that as **inferred**: the readback
format is the only variable between the arms and this is the obvious candidate, but no
measurement here localised where the precision is lost, and the honest alternative - that
the GPU convert shader and ffmpeg's CPU conversion differ in coefficients or rounding -
would produce the same sign. Localising it needs a per-stage comparison nobody has run. The export is 4:2:0 HEVC in the end, which is exactly why
this is easy to get wrong: the final container's chroma format does not tell you where the
subsampling happened. Losing chroma resolution one stage earlier costs real information
that the encoder would otherwise have had.

Of the five metrics, the false-motion doubling is the one that matters most here, and for a
specific reason: the motion field is measured on the output that this change makes
chroma-poorer, so the metric is reading the damage directly rather than at one remove. The
two metrics that move the other way (flicker+ +0.002, sigma+ -0.013) are small beside
-0.75 dB and a 2x on false motion, and neither offsets a fidelity loss of that size.

**This closes the item `GpuPath` flagged as "needs a measurement before it can be
trusted".** The flag pair remains available per-render for anyone who wants throughput over
fidelity: `[Encoding] GpuColorConversion=1` / `GpuSourceConversion=1` in the player ini, the
two Encoder Settings checkboxes, or `--gpu-color-conversion 1 --gpu-source-conversion 1` on
the worker command line. What changes is that the trade now has a price tag on it.

Scope, stated rather than assumed: this A/B is one clip, the 60-frame NR-processed capture
described above, at one resolution. The direction is a precision argument that does not
depend on content - chroma is subsampled earlier, so chroma-detailed material can only lose
- but **the magnitude was not measured on camera-original footage**, and per this repo's
standing rule the -0.75 dB must not be treated as a general size. It is enough to refuse the
flip, which is what it is used for here.

## P5: NVOFA for playback-SR sessions

Implemented. The flow engine is now given the DECODED frame's size instead of the DLSS
input size, and the resolve pass scales vectors per axis into the DLSS input grid:

| site | what it does |
|---|---|
| `src/OpticalFlowNvof.h:142-199` | `HardwareFlowPlan`, `PlanHardwareFlow`, `FlowGeometrySupported` - all constexpr and device-free |
| `src/D3D12Renderer.cpp:511-516` | the rewritten gate comment |
| `src/D3D12Renderer.cpp:527-529` | `PlanHardwareFlow` drives `Initialize`'s geometry |
| `src/D3D12Renderer.cpp:544-554` | the per-session backend log |
| `src/D3D12Renderer.cpp:820-830` | 7 root constants instead of 5 (`MotionScale` added) |
| `src/NvofResolveShader.h:24-32`, `:72` | `float2 MotionScale` in the cbuffer, applied on the return |
| `tests/RenderSettingsTests.cpp:555-612` | unit coverage of both branches and the engine bound |

**Proven on the neural-size path** by a live session log line:

```
Motion guide backend: NVOFA hardware flow on the decoded 1920x1080 frame, vectors scaled by 1,1 into the 1920x1080 DLSS input.
```

The `1,1` is the identity that shows the fast path is byte-unchanged: the scale is
`float(renderW)/float(sourceW)` of equal values, and a multiply by 1 leaves every finite
vector alone, so the emitted motion texture on that path is bit-identical to what shipped
before.

**NOT proven in an actual SR session on this machine.** Enabling `[Playback]
SuperResolution=1` produced `failed to load NGXCore: 126` for `Release\_nvngx.dll` and
`Release\nvngx.dll`, because the player-root Super Resolution runtime is not staged in this
build - the player root is deliberately hook-free, and only `neural-runtime/` is staged. So
the SR branch of `PlanHardwareFlow` is covered by unit tests and by construction, and the
live-session evidence is **owed**. No claim is made here that an SR session was measured,
or that NVOFA was observed selected in one.

What would produce that evidence: stage the SR runtime beside `DLSSVideoPlayer.exe`, re-run
one `player_session.ps1` session with that ini key set, and grep the same
`Motion guide backend:` line for a scale other than `1,1`. That is one session and one
grep; it is not blocked by anything except the staging.

## Verdict

1. **P4's success criterion is UNMET.** The two landed changes are below this machine's
   noise floor at both resolutions (-0.8 % at 1080p, +0.4 % at 4K, spread 0.22 ms and
   0.28-0.44 ms). They stay for the correctness argument - a clear that writes memory the
   next draw overwrites is waste - not for a number.
2. **The change that would meet the criterion must not be made.** +7.0 % throughput costs
   -0.75 dB PSNR, +0.95 dE and 2x false motion, deterministically. `gpuSourceConversion` and
   `gpuColorConversion` stay FALSE, and the flags stay available per-render for callers who
   want the other side of that trade.
3. **Live 4K measurement is blocked** by `#32770|Neural rendering from here`, exit 9, in
   both trees, so the keep-up forecast margin half of P4's criterion has no 4K evidence at
   all. Clearing that dialog is the prerequisite for ever satisfying P4 as written.
4. **P5 is implemented and is byte-neutral on the neural-size path**, evidenced by the
   `scaled by 1,1` log line. The SR-session evidence is owed, and the reason is a staging
   gap, not a code gap.
5. **Row 14 of the audit is handed on**: `m_cacheOutput` is 33 MiB at 4K that is allocated
   unconditionally and never written in NV12 capture. It is a VRAM item, not a readback item.

## What this does not settle (UNEXERCISED)

- **Whether the clear and map removals save anything anywhere.** This record establishes
  only that they are not measurable on an RTX 4080 SUPER at 1080p or 4K. A
  bandwidth-poorer part might show them; that was not measured, and no such claim is made.
- **The conversion trade on camera-original footage.** One 60-frame NR-processed capture at
  one resolution. The sign is argued from chroma precision; the magnitude is not
  generalizable and must not be reused as a size.
- **NVOFA in a Super Resolution session.** Unit-tested and argued from construction, never
  executed. Also unmeasured: the engine's cost when the decoded frame is LARGER than the
  DLSS input, where FAST optical flow is paid at decode resolution rather than at DLSS
  input resolution.
- **The 4K live keep-up forecast margin.** Blocked by the modal dialog, in both trees.
- **The NV12 chroma pass variant** (a MRT form that writes sRGB once instead of converting
  per chroma sample) is not implemented and not measured.
