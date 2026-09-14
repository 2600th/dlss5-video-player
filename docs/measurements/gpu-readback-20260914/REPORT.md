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
| 6 | `src/D3D12Renderer.cpp:1074` `CopyTextureRegion` capture plane -> readback buffer | THE proportional readback: 4 B/px BGRA, 1.5 B/px NV12 | **NECESSARY in kind; its SIZE is a policy - and that policy is now MEASURED: DO NOT FLIP.** The 2.7x is available today without a rebuild, since `request.gpuColorConversion` is also DEFAULT FALSE (`src/OfflineNeuralRenderer.h:49`, `src/main.cpp:1480`, `m_gpuColorConversion=false` at `main.cpp:4626`). Twin of row 1. Measured alone it costs -0.79 dB PSNR and +0.94 dE; `docs/USAGE.md:216-218` already defaults it off on GPU-time grounds that do not reproduce on this card. |
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

**The cause is now attributed, and it is not one flag.** The joint arm flipped two
switches at once, which cannot say which one paid. Both single-flag arms were then
run on the same clip, 3 repeats each. Determinism is from the render side: each arm's
three repeats carry one decoded-frame digest. The quality columns are **n=3 for every
arm except `gpu-source-only`, which is n=1** - the scorer hit its 1800 s ceiling with
that arm's repeats 2 and 3 unscored, and since the renders are digest-identical the
missing rows would repeat the first, but they were not computed and are not claimed:

| arm | PSNR dB | dE mean | flicker+ | sigma+ | false motion | proc fps (median) | gpu_ms_p50 (median) |
|---|---|---|---|---|---|---|---|
| `cpu-conversion` (shipped) | 29.09 | 5.37 | 0.145 | 0.492 | 0.0013 | 31.18 | 18.051 |
| `gpu-color-only` | 28.30 | 6.31 | 0.138 | 0.574 | 0.0017 | 32.98 | 17.849 |
| `gpu-source-only` | 28.45 | 6.24 | 0.155 | 0.396 | 0.0019 | 32.98 | 18.249 |
| both | 28.34 | 6.32 | 0.147 | 0.479 | 0.0026 | 33.35 | 18.355 |

On that untagged 4K clip each flag costs most of the quality on its own - **-0.78 dB
/ +0.94 dE** for the capture side, **-0.64 dB / +0.88 dE** for the decoder side - and
the two together are no worse than either. The non-additivity is what unpicked it,
because two independent precision losses would add.

**Chroma subsampling is not the mechanism.** The clip is `yuv420p`, so the source is
already 4:2:0 and no stage here can lose chroma resolution the input never carried.
The real variable is a colour-matrix disagreement, and it is visible in the clip's
own metadata: `demo-4k-60.mkv` reports `color_space=unknown`, because
`docs/media/neural-comparison-demo.mp4` is itself untagged and `corpus.py`'s encode
adds no `-colorspace`. Both GPU shaders hard-code BT.709 limited range
(`src/D3D12Renderer.cpp:316-341`: `Luma709`, the 16/219 and 128/224 scalings, and
`PSSourceNv12`'s `1.5748`/`1.8556` inverse), while swscale falls back to BT.601
coefficients for a stream that declares nothing. So on untagged content the CPU and
GPU halves of the pipeline are converting under two different matrices.

**The test that settles it: the same pixels, tagged.** Two controls, because the
first changed clip and resolution at once. `orig-faces` is `bt709`/`tv` from its
publisher source. `demo-4k-60-tagged` is the tag-only twin of `demo-4k-60`, built
with `setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv` so
that the encoded YUV is byte-identical - verified, raw `yuv420p` sha256
`6ba4478652286864` on both. A first attempt re-encoded with `-colorspace` flags
instead and silently altered the YUV (`3b95330a...`); it was discarded, because a
twin whose pixels moved is not a control.

One trap to name, since it caused that mistake: `framemd5` in `common.py` decodes to
**rgb24**, so a clip's frame digest legitimately changes when only its tags change -
the tag is what selects the YUV-to-RGB matrix. Pixel identity between tag variants
has to be asserted in the YUV domain, not through the rgb24 digest.

Arm deltas against each clip's own `cpu-conversion`:

| clip | input tags | `gpu-color-only` | `gpu-source-only` |
|---|---|---|---|
| `demo-4k-60` | `unknown` | **-0.783 dB / +0.944 dE** | **-0.638 dB / +0.875 dE** |
| `demo-4k-60-tagged` | `bt709`, identical YUV | **-0.500 dB / +1.060 dE** | **+0.150 dB / +0.270 dE** |
| `orig-faces` (1080p) | `bt709` | **-0.642 dB / +0.644 dE** | **+0.067 dB / +0.009 dE** |

Read the deltas, not the absolutes: tagging changes the matrix the scorer itself
decodes under, so `cpu-conversion` reads 29.09 dB untagged and 27.68 dB tagged on
byte-identical YUV. That is a change of comparison basis, not of quality, and it is
why only within-clip arm deltas mean anything here.

`GpuSourceConversion` goes from **-0.64 dB to free** the moment the input carries
tags, on two clips at two resolutions. Its entire measured penalty was the
601-against-709 mismatch, which is not a readback cost and not shader imprecision:
it is exactly the hazard `docs/USAGE.md:219-221` already names ("assumes BT.709
limited range and nothing reads the source's tags"), now with a number and a
demonstration that reading the tags is the whole fix. On tagged input it is a pure
throughput win.

`GpuColorConversion` keeps **-0.53 to -0.64 dB with the *input* tagged** at both
resolutions - so it is not an input-tagging artifact. It turned out to be an
*output*-side colour-metadata defect all the same: its frames reached the encoder
with no colour properties. See the retraction below.

Chroma siting was the leading suspect - `PSCaptureChroma` converts four RGB samples
and averages the results, which is centre-sited, while swscale's 4:2:0 default is
left-sited. **It was wrong, and the measurement that killed it is below**: once the
NV12 path stamps its frames with all four colour properties, the capture-side cost
goes to **zero** (30.10 dB against the CPU path's 30.10 dB, dE 3.33 against 3.30,
where it had been -0.642 dB / +0.644 dE). Every capture-side number in this report
therefore describes the code *before* that change: the cost was the NV12 path's
frames carrying no primaries or transfer, not the shader's sample positions.

### A defect this A/B walked into, and it is now fixed

The arms' outputs did not carry the same colorimetry, and `src/MediaPipeline.cpp`
said why in so many words: "Only the GPU-converted path states its colorimetry,
because only there does the player choose the matrix. The BGRA path leaves ffmpeg's
own conversion, and its tagging, exactly as they were." Measured before the fix:

| arm | output `color_space` | converted by |
|---|---|---|
| `cpu-conversion` (the shipped default) | **`unknown`** | swscale default |
| `gpu-color-only` | `bt709` | the capture shader, BT.709 |
| `gpu-source-only` | **`unknown`** | swscale default |

A normal render proved it was not an artifact of these profiles: the
`orig-faces__shipped-depth-proxy` output - default flags, a `bt709`-tagged 1080p
source - was `color_space=unknown`.

**Which matrix swscale actually used, measured rather than looked up.** A pure-red
BGRA frame piped through the shipped encoder line and read back as `yuv420p`:

| geometry | default | with `scale=out_color_matrix=bt709:out_range=tv` |
|---|---|---|
| 1920x1080 | Y=81 U=90 V=240 | Y=63 U=102 V=240 |
| 640x480 | Y=81 U=90 V=240 | Y=63 U=102 V=240 |

BT.601 predicts `(81, 90, 240)` for pure red and BT.709 predicts `(63, 102, 240)`,
so this build's swscale takes **BT.601 at both HD and SD** - it does not switch on
resolution, which was worth checking rather than assuming in either direction. The
shipped path therefore took a BT.709 source, converted it with BT.601, and wrote a
file that declared nothing.

**The fix, landed in this wave:** both paths now state `-colorspace bt709`,
`-color_primaries bt709`, `-color_trc bt709`, `-color_range tv`, and the BGRA path
additionally converts with `scale=out_color_matrix=bt709:out_range=tv`. The order
matters: labelling alone would have been worse than the defect, because tagging
601 pixels as BT.709 turns an ambiguous file into a confidently wrong one. The NV12
path takes no filter - its pixels are already BT.709 limited range from the capture
shader, and a scale filter there would put the conversion back on the CPU.

`-color_primaries` and `-color_trc` as *output options* turned out not to survive
on this FFmpeg (9.0.1): only the matrix and range landed, in Matroska and MP4 alike,
with NVENC and with x264, and an `hevc_metadata` bitstream filter did not help
either. Setting them on the frames does work, so the BGRA path carries
`setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709:range=tv` after
the scale. Measured pixel-safe there: scale-only and scale+setparams decode to
identical planes and only the tags change.

The same filter is **not** on the NV12 path, and that is a measured decision rather
than caution: adding it changes that path's decoded output on this build,
reproducibly, with NVENC confirmed deterministic over repeat encodes (identical
input twice gives identical planes). A path whose entire purpose is to reach the
encoder untouched does not get a filter that touches it, so it keeps the matrix and
range - the two tags that decide whether colours come out right - and leaves
primaries and transfer unstated until that is understood.

**Verified end to end on identical input**, the same 4K clip rendered by the pre-fix
and post-fix workers:

| | output tags | plane sha256 | mean Y | mean U | mean V |
|---|---|---|---|---|---|
| pre-fix | `tv`, everything else `unknown` | `4aea41a439e7cfec` | 57.12 | 124.58 | 123.51 |
| post-fix | `tv`, `bt709` x3 | `bac1336bc4cb1fbc` | 58.41 | 124.03 | 123.15 |

So the pixels really moved (+1.29 mean Y), which is the point: the fix is a
conversion change and not only a relabelling. And the arm delta is **unchanged**
(`cpu` 30.10 vs `gpu-color-only` 29.45, still -0.65 dB), exactly as the section
below predicts - each path round-trips under its own tags, so PSNR against the
source is blind to the matrix in both states. The case for the fix is the file being
labelled for what it contains, not a metric moving.

One more consequence that needed code: the encoder arguments are deliberately not
part of the render identity, so cached renders written before this change - BT.601
pixels, no tags - would have stayed valid hits under an unchanged `VERSION`. The
identity's pipeline term now carries `bt709-export-v1`, which retires them.

Two consequences worth separating. First, this was a correctness defect in the
default export path, independent of the throughput question and worth more than it:
it affected every neural render, not only the ones made with an experimental flag.
Second, stamping the same four properties on the NV12 path's frames **removed the
capture-side quality cost entirely**, which retires the chroma-siting hypothesis
before it was ever tested:

| `orig-faces`, both paths post-fix | PSNR dB | dE mean |
|---|---|---|
| `cpu-conversion` | 30.10 | 3.30 |
| `gpu-color-only` | 30.10 | 3.33 |

A 0.00 dB delta where it had been -0.642 dB. The mechanism is the same one this
whole section is about, one level down: the NV12 frames reached the encoder with no
primaries or transfer, and NVENC encoded them differently for it. The direction is
measured on a flat synthetic frame: without the properties the decode comes back
Y +3 and V -2 against the input (120 -> 123, 200 -> 203), and with them it
reproduces the input exactly. That is *not* a range conversion - full-to-limited
would map 120 -> 119.1 and 200 -> 187.8, limited-to-full 121.1 and 214.2 - so the
cause sits inside the ffmpeg-to-NVENC path for frames whose colour properties are
unspecified, and it is not isolated further than that. What is certain is the
direction and the fix: described frames encode to the input, undescribed ones do
not. `setparams` is metadata-only and pixel-exact
through a lossless round trip, so nothing about the pixels handed to NVENC changed -
what changed is that they are now described. Measured against the source directly,
the tagged encode is 0.66 dB closer (32.65 against 31.99 dB by FFmpeg's own `psnr`
filter), which is the same 0.65 dB that used to look like a shader defect.

Per-channel signed error (`rgb_shift`, mean output-minus-source, 8-bit levels) shows
the same split - every arm carries the relight's own large bias, but only on the
untagged clip do the GPU arms swing it:

| clip | arm | r | g | b |
|---|---|---|---|---|
| `demo-4k-60` | cpu | -9.307 | +1.552 | +3.022 |
| `demo-4k-60` | `gpu-color-only` | -8.600 | +2.099 | +3.652 |
| `demo-4k-60` | `gpu-source-only` | -10.609 | +3.121 | +5.946 |
| `orig-faces` | cpu | -2.455 | -2.471 | -1.490 |
| `orig-faces` | `gpu-color-only` | -3.176 | -4.106 | -0.562 |
| `orig-faces` | `gpu-source-only` | -1.358 | -0.892 | -0.542 |

**So the verdict splits, and neither half is what this report first concluded.**

`GpuColorConversion` has **no measured quality cost** once the colour fix is in:
0.00 dB against the CPU path, +5.8 % processing throughput at 4K, and on this card
it *lowers* `gpu_ms_p50` (17.849 against 18.051) where `docs/USAGE.md:216-218`
defaults it off on GPU-time grounds measured on an RTX 5070 Ti. That leaves it a
candidate for defaulting on, blocked by one thing only: the GPU-time argument was
measured on a different card and this one contradicts it, so the honest next step is
re-measuring that, not flipping on one machine's numbers. Not flipped in this wave.

`GpuSourceConversion` is free on **correctly tagged** input and its blocker is
untouched by any of this. The colour fix states the *output's* colorimetry; it does
not read the *source's*. A BT.601 or full-range source still meets a shader that
assumes BT.709 limited range, which is exactly the hazard
`docs/USAGE.md:219-221` names and exactly what the -0.64 dB on the untagged clip
measures. That flag needs the source colour-tag probe before it can be a default,
and that remains a correctness item rather than a performance one.

So: both defaults stay today, for two different reasons, and neither reason is the
one this report opened with.

**This measurement is a confirmation, not a discovery, and the record already said
so.** `docs/USAGE.md:214-221` documents both flags as deliberately off, with reasons:
the capture side because the GPU is the scarce resource under the neural pass (8.35
against 8.66 ms/frame on an RTX 5070 Ti), the decoder side for the colour-tag hazard
above. One of those two reasons does not reproduce here: on this 4080 SUPER at 4K the
capture-side flag *lowers* `gpu_ms_p50` (17.849 against 18.051). That is a
machine-and-resolution difference, not a contradiction to assert over the shipped
note - but it means the cost argument for that flag is card-dependent while the
quality argument measured here is not. The decoder-side flag's real blocker remains
the missing colour-tag probe, which is a correctness item and not a readback one.

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
