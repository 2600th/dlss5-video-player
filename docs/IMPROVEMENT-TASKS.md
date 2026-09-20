# Improvement task list

_Audited against 0.24.0 (dec9a87) on 2026-09-20._

Findings from a six-agent review: two competitive/baseline web surveys, three
code audits (correctness, performance, build & release), one duplication sweep.

**Confidence marks on every task:**

| Mark | Meaning |
| --- | --- |
| ✅ | Read back against source and confirmed during the review |
| 🔍 | Audit finding, cited to file:line, not independently re-read |

**Checkbox state:** `[x]` done on branch `fix/tier1-correctness-and-perf`, `[~]`
partly done (what remains is stated in the task), `[ ]` not started. All 21
tests - 13 portable and 8 GPU-labelled on an RTX 5090 - pass at every commit.

**Measured on this machine** (2560x1440 23.976 fps, plain playback), before
and after the branch:

| | main (ee1f393) | branch |
| --- | ---: | ---: |
| CPU during playback | 113% of one core | 9.5-17.8% |
| Guide work per frame, SR off | 0.84 ms | 0.0001 ms |
| NV12 comparison conversion | 66.11 ms/frame | 5.51 ms/frame |
| Full clean rebuild, all targets | 103.2 s | 52.4 s |
| Presented vs source | not reported | 23.83-24.43 of 23.9794, dropped=0 |

---

## The rule that shapes this list

> **Quality is the default. It is never traded for speed without the user
> asking.**

Every optimisation below is sorted into one of two piles.

**Quality-neutral** — pure waste. Redundant copies, busy-spins, dead GPU
passes, allocation churn, polling. Output is byte-identical.
→ **Ship unconditionally. No setting, no prompt.**

**Quality-affecting** — processing scale, NVENC preset, multi-pass, temporal
blend.
→ **Named ladder. Default to the near-best rung. Print the measured cost next
to each rung. Label the default as the recommended one.**

Good news: **the entire performance section is quality-neutral.** ~5-9 ms/frame
at 1440p with identical pixels.

One blocker sits directly on this rule and is listed first in Tier 1.

---

## Contents

| Tier | Theme | Tasks | When |
| --- | --- | --- | --- |
| [1](#tier-1--now) | Correctness + free wins | 12 | Days |
| [2](#tier-2--next) | Perf, build, tests | 23 | Weeks |
| [3](#tier-3--strategic) | Features and positioning | 14 | Months |
| [4](#tier-4--parked) | Deliberately not doing | 8 | Never |

**Do not re-suggest:** [What is already strong](#what-is-already-strong).

---
---

# Tier 1 — Now

Correctness bugs and zero-cost wins. Nothing here changes output quality.

---

### [x] 1.1 · Cache key is missing two settings that change the pixels

`BLOCKER` · ✅ verified · **effort: S** · **impact: high**

**This blocks every quality feature below.** Fix it first.

**Where** — `src/NeuralCache.cpp:573`

```cpp
std::string NeuralRenderPipelineIdentity(bool gpuSourceConversion)
{
    std::string pipeline =
        "DLAA|strict-timeline-v3|armed-inline-interception-v3|bt709-export-v1";
    if (gpuSourceConversion) pipeline += "|nv12-source-v1";
    return pipeline;
}
```

**What's wrong** — only the *source* conversion is keyed. Two other settings
reach the render at `src/main.cpp:5791` and both change the encoded pixels:

- `nvencPreset` — picks the NVENC preset. Your own tooltip quantifies it:
  *"p7 takes twice the encode time of p5 and buys 0.12 VMAF."*
- `gpuColorConversion` — picks between a GPU 2x2 box chroma downsample and
  ffmpeg's CPU conversion. Two different filters over the neural output.

**Failure** — set Encoder settings to p7 ("Applies to the next render"),
re-render the same range → cache hit → you get the p5 file forever. The
validity re-check at `main.cpp:5735` tests the same terms, so it agrees.

**Fix** — append both to `NeuralRenderPipelineIdentity`, bump the key schema
from `"2"` to `"3"` at `NeuralCache.cpp:585`.

---

### [x] 1.2 · Heap over-read of ~5 MB on the DLSS Upscaling toggle

`CRITICAL` · ✅ verified · **effort: XS** · **impact: crash**

**Where** — `src/main.cpp:3691`

```cpp
if(candidate->guides.Generate(m_lastPlaybackFrame.bgra.data(),
    m_decoder.Width(),m_decoder.Height(),
    m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate(),
    IdentityOf(...),guide)){            // <-- 8 args, no layout
```

**What's wrong** — the `layout` parameter defaults to
`SourcePixelLayout::Bgra` (`src/TemporalGuides.h:98`). But playback opens with
`preferNv12=true` (`main.cpp:3544`), so the buffer is NV12 — `w*h*3/2` bytes.
The guide reader indexes to `(w*h-1)*4`.

At 1080p: **byte 8,294,396 of a 3,110,400-byte allocation. 5.2 MB past the
end.** A 3 MB vector gets its own VirtualAlloc region, so this lands in
unmapped space with high probability.

The sibling call site 96 lines later gets it right *and warns about this exact
mistake* — `src/main.cpp:3784`:

```cpp
// The frame says which layout it is in; the guide generator has read
// both since the export path started decoding to NV12, and reading a
// NV12 buffer as BGRA is the kind of mistake that shows up as motion
// estimated from noise rather than as a failure.
if(!m_guides.Generate(f.bgra.data(), ..., g, f.layout)) return false;
```

**Repro** — open any YouTube video (BT.709 limited → NV12), press the DLSS
Upscaling toggle. Also reached from `SetUpscaleTarget` and `RestoreUpscaling`.

**Fix**

1. Pass `m_lastPlaybackFrame.layout`.
2. Call `ConfigureRendererSource()` on the candidate before `Initialize` —
   currently never done, so playback SR can never engage on an NV12 source
   even if the read survives.
3. Harden: give `Generate` the buffer size and refuse a mismatch rather than
   trusting the caller.

---

### [x] 1.3 · Permanent deadlock on a resident helper's second job

`HIGH` · ✅ verified · **effort: XS** · **impact: hang**

**Where** — `src/OfflineNeuralRenderer.cpp:1735`

```cpp
void Shutdown(){
    if(!m_worker.joinable())return;
    {std::scoped_lock lock(m_mutex);m_quit=true;}   // never reset
    m_wake.notify_one();
    m_worker.join();
}
```

**What's wrong** — `m_quit` is latched and never cleared. Traced sequence:

1. Job 2's `Initialize` → `Release` → `Shutdown`. Sets `m_quit=true`, joins.
2. First `Post` sees `!joinable()`, starts a thread. It does **one** copy,
   re-waits, sees `m_quit` still true, and **exits** at `:1749`.
3. `m_worker` is now joinable-but-finished.
4. Second `Post` sees `joinable()==true` → starts nothing → sets `m_busy=true`
   → notifies no one.
5. `Join` blocks forever on `m_idle.wait(lock,[this]{return !m_busy;})`.

**Repro** — render a 1080p range, then a 1440p range in the same session
(any geometry / fps / layout / colour / capture-format change triggers the
`Release`). Helper wedges on the third captured frame holding the D3D12 device
and ~1 GiB of DLSS feature memory. `Pump` has no overall timeout.

**Fix** — reset `m_quit=false` in `Post`, and `join()` the finished thread
before restarting it.

---

### [x] 1.4 · A stalled audio clock freezes video permanently

`HIGH` · 🔍 reported · **effort: S** · **impact: playback stops**

**Where** — `src/AudioPlayer.cpp:226`

```cpp
const auto state=m_reader;
if (!state || !state->waveOut || !state->hasAudioData.load()) return -1.0;
```

**What's wrong** — `hasAudioData` is set once at `:191` and cleared only in
`Stop()` at `:276`. When the reader thread ends (pipe EOF, ffmpeg child death,
`waveOutWrite` failure at `:201`) the queued buffers drain, `waveOutGetPosition`
stops advancing, and `PositionSeconds()` keeps returning the frozen value.

That value is the master clock — `src/main.cpp:3896`:

```cpp
double audio=Audio().PositionSeconds();
if(audio>=0.0){double d=m_decoder.DurationSeconds();
               return d>0?std::clamp(audio,0.0,d):audio;}
```

and the presentation gate at `main.cpp:1315` is `if(now+0.001<due) return;`.

**Repro** — a 60 s video with a 10 s audio track. At ~10.7 s the clock sticks
and **video stops for the remaining 50 s**. No error, no log, no fallback to
the steady clock sitting three lines below. Unplugging a USB headset
mid-playback does the same via `MMSYSERR_NODRIVER`.

**Fix** — treat a clock that has not advanced for >N frame intervals as dead:
clear `hasAudioData`, log once, fall through to the steady-clock branch.

**Pairs with 1.5** — right now the cause is completely invisible.

---

### [~] 1.5 · ffmpeg stderr goes to NUL everywhere; audio exit code never checked

`MEDIUM` · 🔍 reported · **effort: S** · **impact: undiagnosable bugs**

**Where** — `src/VideoDecoder.cpp:358`, `:762`, `src/AudioPlayer.cpp:90`

All three set `si.hStdError = nul;` and run with `-loglevel error`. The only
diagnostic the child produces is discarded. The audio child's exit code is
never queried (`AudioPlayer.cpp:182` just breaks).

**Why it matters** — an ffmpeg audio failure is 100% invisible: no stderr, no
exit code, no log. And per 1.4 it manifests as frozen *video*, so the symptom
points at the wrong subsystem.

**Fix** — capture stderr to the log (rate-limited), and check the exit code.

---

### [x] 1.6 · A transient ffprobe failure deletes a hash-verified cache entry

`HIGH` · 🔍 reported · **effort: S** · **impact: data loss**

**Where** — `src/main.cpp:5735`

```cpp
const bool valid=cachedProbe.ok&&cached->manifest.sourceDigest==*sourceDigest&&...;
if(valid){ ... goto finish;}
if(!cache.Quarantine(*cached)){...}
```

**What's wrong** — `cached` already passed a full SHA-256 of the payload plus
both sidecars inside `LookupRender` (`NeuralCache.cpp:973`). `ProbeMedia` then
fails closed for reasons unrelated to the entry — including the helper simply
not resolving (`MediaPipeline.cpp:1079`):

```cpp
if (ffprobe.empty() ...) { result.detail = L"FFmpeg tools are unavailable."; return result; }
```

`Quarantine` moves it to `staging/invalid-cache-*`, and `SweepStaging` deletes
any `invalid*` name unconditionally on the next manager construction
(`NeuralCache.cpp:866`).

**Repro** — antivirus locks or quarantines the unsigned `ffprobe.exe`. User
opens each of their five rendered videos. Every probe fails. Within a few
manager constructions, **hours of GPU time — hash-verified as intact — are
deleted.** Same pattern destroys downloaded source copies at `main.cpp:1128`.

**Fix** — distinguish *"the probe could not run"* from *"the probe disagrees"*.
Only quarantine on the latter.

---

### [x] 1.7 · Adopting the local copy flips NV12 → BGRA under an NV12 renderer

`HIGH` · 🔍 reported · **effort: XS** · **impact: corrupted picture**

**Where** — `src/main.cpp:4560`

```cpp
VideoDecoder local;
if(!local.Open(copy->wstring(),MediaSourceKind::LocalFile)){ ... }
if(local.NativeWidth()!=m_decoder.NativeWidth()||
   local.NativeHeight()!=m_decoder.NativeHeight()){ ... }   // geometry only
...
m_decoder.Swap(local);local.Close();
```

**What's wrong** — geometry is checked, **layout is not**. `Open`'s
`preferNv12` defaults to `false` (`VideoDecoder.h:131`) while the streaming
open used `true`. `Swap` swaps the whole `m_source` including layout
(`VideoDecoder.cpp:166`), and nothing afterwards calls
`ConfigureRendererSource()`.

**Repro** — play a YouTube video, let background acquisition finish, then seek
(`main.cpp:3931`) or hit live-session stall recovery (`:2457`, `:3976`).
Renderer's `m_sourceLayout` is `Nv12`, frames arrive as BGRA. The size check
passes (`w*h*4 >= w*h*1.5`), so the NV12 path uploads the first quarter of the
BGRA image as a Y plane. **Severely corrupted picture, no error anywhere.**

**Fix** — open the copy with `preferNv12` matching `m_decoder.PixelLayout()`,
and refuse the swap on a layout mismatch the way geometry already does.

---

### [x] 1.8 · `remove_all` runs against a relative path when there is no cache root

`MEDIUM` · 🔍 reported · **effort: XS** · **impact: deletes user data**

**Where** — `src/main.cpp:4938`

```cpp
m_liveDirectory=m_cacheRoot/L"live";
std::error_code ec;std::filesystem::remove_all(m_liveDirectory,ec);
```

**What's wrong** — `m_cacheRoot` is assigned only inside `if(historyCache.Valid())`
(`main.cpp:1191`) and is empty in the automatic configuration (`:2667`, `:2677`).
`LiveSessionAvailable()` → `RangeRenderAvailable()` (`:4652`) does not check
cache validity. So `m_liveDirectory` becomes the relative path `"live"`.

**Repro** — portable install on a read-only share with LocalAppData redirected
away (the exact case `PrepareWritableRoot`'s fallback exists for). Toggling
neural rendering **recursively deletes whatever `live` directory sits in the
process working directory.** Same at `ReleaseLiveSession`, `:4985`.

**Fix** — refuse to build a live directory from an empty root; assert the path
is absolute before any `remove_all`.

---

### [x] 1.9 · Two player instances share `<cacheRoot>/live` and delete each other's work

`HIGH` · 🔍 reported · **effort: S** · **impact: data loss**

**Where** — `src/main.cpp:4938` (the `remove_all` above) and `:5587`

```cpp
const uint64_t liveRunId=liveIndex?uint64_t(++m_liveJobSerial):0u;  // per-process
liveDirectory=m_liveDirectory/(L"job"+std::to_wstring(liveRunId));
```

**What's wrong** — the run id is a per-process counter and there is no
single-instance guard. The named-mutex lease covers the runtime directory, not
the cache root (`NeuralWorker.cpp:1641`).

**Repro** — player open on two videos. Instance A is 4 minutes into a live
session with 40 finalized segments. User enables neural rendering in instance
B → B's `remove_all` deletes A's segments. A's `NeuralSegmentIndex` is pure
in-memory arithmetic and still reports them covered, so A seeks into "covered"
ground and fails to open the file. Both then write `job1/neural-00000.mkv`
into the same directory.

**Fix** — derive the live directory from the process id or a GUID. Never
`remove_all` a shared path.

---

### [ ] 1.10 · Restore the deleted release script

`BLOCKER` · ✅ verified · **effort: XS**

`tools/package_release.ps1` is **deleted in the working tree, uncommitted**
(`git status` → ` D tools/package_release.ps1`).

Referenced by:

- `package_release.bat`
- `package_public_release.bat`
- `.github/workflows/release.yml:121`
- `docs/BUILDING.md:161`

**Fix** — `git restore tools/package_release.ps1` before pushing anything.

---

### [x] 1.11 · Make a bug report possible at all

`HIGH` · 🔍 reported · **effort: M** · **impact: every future debug session**

Three separate defects that compound.

**(a) The log is destroyed on restart** — `src/Log.h:33`

```cpp
Log() : m_file(LogPath(), std::ios::out | std::ios::trunc) {}
```

User crashes → relaunches to collect the log → **evidence gone**. And
`.github/ISSUE_TEMPLATE/bug_report.yml:38` asks for exactly that file.

**(b) No fallback, no failure check** — `LogPath()` (`:26`) returns the module
directory only. `Write()` (`:14`) never tests `m_file`. `NeuralCache.cpp:791`
*does* fall back to `%LOCALAPPDATA%`, so in a Program Files install the cache
works and the log silently vanishes — the exact scenario
`docs/TROUBLESHOOTING.md:127` warns about.

**(c) No crash dumps** — `grep -E "MiniDumpWriteDump|SetUnhandledExceptionFilter|AddVectoredExceptionHandler|__try" src/`
returns **zero hits**. An access violation produces no artifact at all.

**Also:** 13 large modules log nothing, including all **1,143 lines of
`YouTubeResolver.cpp`** — the component most exposed to upstream breakage.
Also `MediaPipeline.cpp`, `SynchronizedPlayback.cpp`, `NeuralWorker.cpp`,
`RuntimeLock.cpp`.

**Fix**

- [ ] `ios::app` + a session banner instead of `ios::trunc`
- [ ] `%LOCALAPPDATA%` fallback on open failure, mirroring `NeuralCache.cpp:791`
- [ ] Cap and roll at ~8 MB
- [ ] `SetUnhandledExceptionFilter` + `MiniDumpWriteDump` in `wWinMain`
      (`main.cpp:7010`) **and** in `NeuralWorkerMain.cpp`, writing beside the log
- [ ] Add `LOG` lines to `YouTubeResolver.cpp` — every refusal path currently
      returns the same opaque string for five distinct causes

---

### [~] 1.12 · Attest the package users actually download

`CRITICAL` · 🔍 reported · **effort: M** · **impact: trust**

`README.md:33` labels the 308 MB complete package **"This is the one you
want."** It contains the unsigned, signature-stripped neural DLL.

`release.yml:98` attests **only** the core zip:

```yaml
- name: Attest core package provenance
  uses: actions/attest-build-provenance@4d101475... # v4.2.2
  with:
    subject-path: dist/DLSSVideoPlayer-v*-core-win64.zip
```

Checked live against the GitHub API for v0.24.0:

| Asset | Downloads | Attestations |
| --- | ---: | ---: |
| `DLSSVideoPlayer-v0.24.0-core-win64.zip` | 0 | **1** |
| `dlss5-video-player-v0.24.0-win64.zip` ← recommended | 7 | **404 — none** |

The complete zip was uploaded at `19:35:25Z`, 8 minutes after the release job
finished at `19:27:33Z` — by hand, exactly as `release.yml:118` describes. Its
only integrity claim is a `.sha256` from that same machine, which proves
nothing to a third party.

**Compounding it** — `verify_package.ps1` is **not in its own `$expected`
allowlist** (`tools/verify_package.ps1:17`), so it does not ship inside the
zip. A user cannot verify what they downloaded without cloning the repo. And
`README.md` never mentions `gh attestation verify`.

**Fix**

- [ ] Move the complete-zip build into `release.yml` behind a gated job, **or**
      add a `workflow_dispatch` "attest an uploaded asset" job
- [ ] Add `verify_package.ps1` to `$expected` so it ships inside the zip
- [ ] Add a `gh attestation verify` + `sha256sum -c` block to `README.md`
      under `## Download`

---
---

# Tier 2 — Next

Performance, build, and test work. **Every perf item is quality-neutral:
byte-identical output.**

---

## 2A · Performance

Total recoverable: **~5-9 ms/frame at 1440p**, more at 4K.

Baseline for judging these: 16.68 ms at 59.94 fps, 8.34 ms at 119.88 fps.

---

### [ ] 2.1 · Four full-frame CPU deep copies per presented pair

🔍 reported · **effort: M** · **gain: 1.8-2.8 ms/pair @1440p, 4.1-6.2 ms @4K**

`VideoFrame` holds `std::vector<uint8_t> bgra` (`VideoDecoder.h:44`), so every
`=` is a deep copy.

| # | Where | Line |
| --- | --- | --- |
| 1 | out of the synchronized pair | `main.cpp:2435` — `m_next=*visible;` |
| 2 | inside `RenderVideoFrame` | `main.cpp:3807` — `m_lastPlaybackFrame=f;` |
| 3,4 | `RememberRenderedCachedPair` | `main.cpp:2595` — both members |

**Cost** — 4 × 5.53 MB = **22.1 MB memcpy/pair at 1440p**; 49.8 MB at 4K. At
8-12 GB/s that is 11-17% of budget at 59.94 fps and **22-34% at 119.88 fps** —
the exact rate `PlaybackCadence.h` was written to survive. Also evicts L2/L3
four times per frame, taxing the guide pass and `CopyMappedRows` that follow.

**Fix** — all four consumers want *a frame that outlives the pair*, not a
private copy. Hand out `std::shared_ptr<const VideoFrame>` from
`SynchronizedPlayback`. Three of the four are pure aliasing.

**Start here (lowest risk):** delete copy 2 and have `RecoverUnusableRenderer`
re-read from the pair.

---

### [~] 2.2 · Main loop busy-spins at 100% of a core, redoing work each spin

🔍 reported · **effort: M** · **gain: one core + lock contention**

`src/main.cpp:1336`

```cpp
DWORD TickSleepMs()const{
    return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;
}
```

While playing this is `Sleep(0)` — a yield, not a block. `Tick` returns
immediately when the frame is not due, so the loop free-runs.

**What each wasted iteration costs:**

| | Cost |
| --- | --- |
| **(a)** `LiveCoverage()` (`main.cpp:4747`) | Allocates + `MergeSpans` sorts already-sorted data under `mutex_`, **2-4× per tick**. A 5-minute session at 2 s/segment holds ~150 segments. Hammers the same mutex the metadata reader needs to append. |
| **(b)** `PositionSeconds()` (`AudioPlayer.cpp:225`) | `waveOutGetPosition` under `waveMutex` — the same lock the audio feeder holds across `waveOutWrite`. **≥4× per tick.** |
| **(c)** YouTube only (`main.cpp:4466`) | Constructs a `NeuralCacheManager` **twice per tick**: `weakly_canonical`, **five** `create_directories`, a `CreateFileW` probe, and a full `directory_iterator` sweep of `staging/`. The SHA-256 memo at `:4494` sits *after* the construction, so none of it is avoided. |

**Fix** — the mechanism already exists in this codebase. `PrecisionSleeper`
(`VideoDecoder.cpp:41`) wraps `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
sub-millisecond, without touching global timer resolution.

- [ ] Replace `Sleep(TickSleepMs())` with `MsgWaitForMultipleObjectsEx` on that
      timer armed to `min(due - now, 2 ms)` — see **2.6** for the better signal
- [ ] Cache `CoveredRanges()` against `m_liveSegments->Revision()` (already
      tracked at `main.cpp:5311`)
- [ ] Hoist the `NeuralCacheManager` construction behind the memo

---

### [x] 2.3 · Comparison-while-playing collapses to a slideshow

🔍 reported · **effort: M** · **gain: 18-37 ms/frame @1440p** · `REGRESSION`

**Where** — `src/main.cpp:80`

```cpp
const double luminance = (double(luma[size_t(y) * width + x]) - 16.0) / 219.0;
const double blueDiff  = (double(chromaRow[(x & ~1u)])      - 128.0) / 224.0;
const double redDiff   = (double(chromaRow[(x & ~1u) + 1u]) - 128.0) / 224.0;
...
const auto clamp8 = [](double value) {
    return uint8_t(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
};
```

Scalar, `double`-precision, single-threaded, three `std::lround` per pixel.

Its own comment at `:2899` says it is a **paused-inspection** path — *"a paused
inspection, where a CPU pass over one frame costs nothing anyone can
perceive."* But the call site is **unconditional** inside `RenderVideoFrame`
(`main.cpp:3801`), gated only by the pure-neural early-out at `:2903`.

**So it runs every presented frame the moment a viewer picks Blend / Split /
Wipe, or moves strength off 1.0.**

3.69 Mpx × ~20-40 cycles at 4 GHz = **18-37 ms/frame at 1440p**, 41-83 ms at
4K. Cannot fit a 16.68 ms budget.

**This is a new exposure created by 0.24.0's NV12 playback change** — before
it, `original.bgra` was already BGRA and `UploadReferenceFrame` took the
pointer directly (still the case at `main.cpp:2911`).

**Fix** — the GPU already has the shader (`PSSourceNv12`,
`D3D12Renderer.cpp:770`) and the two-plane upload machinery. Add a second plane
pair for the reference and convert on the GPU.

**Interim (~1 hour, 20-40× faster):** `float` not `double`, int truncation with
a 0..255 LUT instead of `std::lround`, and wrap the row loop in
`ParallelForRanges` like `CopyMappedRows` already does.

---

### [x] 2.4 · Guides and two full-res GPU passes run every frame with DLSS off

🔍 reported · **effort: S** · **gain: 0.6 ms/frame CPU + ~44 MB/frame GPU**

The player calls `SetDLSS(false)` on every media load (`main.cpp:3845`,
`:5932`, `:6129`, `:3553`) and the runtime SR toggle defaults off.

Yet:

- `main.cpp:3787` generates guides **unconditionally**
- `D3D12Renderer.cpp:943` and `:976` record the expansion and depth passes
  **unconditionally**

The only consumers of `m_motion` / `m_depth` are `m_dlss.Evaluate` (`:1039`)
and the debug views. **NVOF is already correctly gated** at `:916`:

```cpp
const bool nvofFrame=motionGuides&&m_nvofActive&&DLSSEnabled();
```

The CPU estimator and the two GPU passes are not.

**Cost** — your own playback-health line measures `guide=0.6 ms` at 1440p.
That is 3.6% of budget at 59.94 fps, **7.2% at 119.88 fps**, all wasted in the
default state and in *every* cached-pair session. GPU side: motion 14.7 MB +
depth clear 14.7 MB + depth write 14.7 MB ≈ 0.15 ms on a 288 GB/s card.

**Also** — `Generate` reads every source pixel on the CPU and allocates ~8
fresh `std::vector<float>` per call (`TemporalGuides.cpp:539`, `:589`, `:439`).
`out.guideGridRGBA32F.assign(...)` zero-fills 230 KB that `:603` then
overwrites element by element, and `GuideFrame g;` at `main.cpp:3774` is a
fresh local — a real 230 KB malloc every frame.

**Fix**

- [ ] Gate `m_guides.Generate` on `m_renderer->DLSSEnabled()`, substituting a
      zeroed grid and `hasHistory=false`
- [ ] Gate the two guide passes on `DLSSEnabled() || m_debugView != Final`
- [ ] Promote the five `Generate` scratch vectors to generator members
- [ ] Keep a reusable `GuideFrame` in `PlayerApp`

Guide history already resets when SR is toggled on, so no extra work there.

---

### [~] 2.5 · 177 MB (1440p) / 398 MB (4K) allocated and never touched

🔍 reported · **effort: S** · **gain: see table**

All allocated unconditionally in `CreateVideoResources`, in both processes.

| Resource | Where | 1440p | 4K | Heap |
| --- | --- | ---: | ---: | --- |
| `m_cacheOutput` | `D3D12Renderer.cpp:694` | 14.7 MB | 33.2 MB | VRAM |
| `m_cacheReadback[4]` | `:755` | 59.0 MB | 132.8 MB | host READBACK |
| `m_reference` | `:768` | 14.7 MB | 33.2 MB | VRAM |
| `m_referenceUpload[6]` | `:770` | 88.5 MB | 199.1 MB | host UPLOAD |
| | **total** | **177 MB** | **398 MB** | |

- The BGRA capture target and readback ring are used **only** by
  `OfflineNeuralRenderer` — the player never calls
  `EnqueueEvaluatedFrameCapture`.
- `FrameCount = 6` exists for the *export* path (`D3D12Renderer.h:334`: *"An
  export source frame records two command lists… Six allocators keep three
  complete source frames in flight."*). The player records one list per frame
  and needs ~3.
- The SR validation path builds a **second** renderer on a child window
  (`main.cpp:6129`), so peak doubles during a toggle.

**Note** — your own audit parked this as row 14, but that row covered only
`m_cacheOutput`. The readback ring and the six reference uploads were out of
that slice's scope.

**Fix** — three independent low-risk changes:

- [ ] Add `bool capture` to `Initialize`, defaulted off, set by
      `OfflineNeuralRenderer.cpp:1829`. Skips `m_cacheOutput` + `m_cacheReadback[]`.
- [ ] Allocate `m_reference` + uploads lazily on the first non-neural
      comparison mode
- [ ] Cut reference uploads from 6 to 3

---

### [x] 2.6 · `SetMaximumFrameLatency(2)` is a silent no-op

🔍 reported · **effort: S** · **gain: small directly, unlocks 2.2**

**Where** — `src/D3D12Renderer.cpp:230`

```cpp
sd.Flags = m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
...
// :233
if(m_swapchain) m_swapchain->SetMaximumFrameLatency(2);
```

`IDXGISwapChain2::SetMaximumFrameLatency` is valid **only** on a chain created
with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`. Otherwise it returns
`DXGI_ERROR_INVALID_CALL` — and the return value is discarded. Latency stays at
DXGI's default of 3.

**Why it matters more than it looks** — `GetFrameLatencyWaitableObject()` gives
a handle the main loop can wait on. That is the *correct* fix for **2.2** and
removes the need to guess a sleep interval.

**Fix**

- [ ] Add the flag at `:230` (compatible with `ALLOW_TEARING`)
- [ ] Check the HRESULT at `:233`
- [ ] Store `GetFrameLatencyWaitableObject()`, wait on it in `wWinMain`
- [ ] Pass the flag in the resize path too

---

### [ ] 2.7 · 682 ms of `waveOut` buffering sets the seek cost

🔍 reported · **effort: S cheap / L structural**

**Where** — `src/AudioPlayer.cpp:157`

```cpp
constexpr size_t BufferCount = 8;
constexpr size_t BytesPerBuffer = 16384; // ~85 ms stereo/48k/16-bit
```

8 × 85.3 ms = **682 ms of queued PCM** over legacy MME, with the master clock
read from `waveOutGetPosition` and **no device-latency compensation**.

A seek tears down and respawns both ffmpeg children (`main.cpp:3940`, `:3995`).
`Stop` waits up to 200 ms twice; `Start` must fill ≥1 × 85 ms buffer before
`hasAudioData` goes true.

This is the measured half-second `PlaybackCadence.h:63` prices RE-ANCHOR
against:

```cpp
// Lateness that says walking has lost - seek instead. One second is about two
// seek costs here, so it is the point where covering the gap frame by frame
// stops being the cheaper way to close it.
inline constexpr double kReanchorSeconds = 1.0;
```

**Secondary** — `waveOutGetPosition` quantizes to the shared-mode engine period
(~10 ms), which is **larger than a frame interval at 119.88 fps**. Several
frames become due at once, then none.

**Fix**

- [ ] **Cheap:** drop `BytesPerBuffer` to 4096 (171 ms of queue). Removes ~64 ms
      of prefill from every seek. Re-measure the "Seek timing" log line — this
      changes the cadence policy's whole cost model.
- [ ] **Structural:** WASAPI shared-mode with `IAudioClock::GetPosition` +
      `GetFrequency` + `IAudioClient::GetStreamLatency`. Sub-ms,
      latency-corrected clock, and a seek flushes in one call instead of a
      process respawn. See **3.7**.

---

### [ ] 2.8 · Parent-side IPC polls at 20 ms while the helper side is event-driven

🔍 reported · **effort: M** · **gain: ~10 ms mean off every segment handoff**

**Where** — `src/NeuralWorker.cpp:543`

```cpp
const DWORD wait = WaitForSingleObject(helper.process, 20);
if (wait != WAIT_TIMEOUT) { outcome.exited = true; break; }
```

The process handle only signals on exit, so in steady state this **always**
times out. Every `Progress`, `Segment`, `Result` and `Timeline` message is
noticed 0-20 ms late. A `Segment` message is what makes a finalized
`neural-NNNNN.mkv` visible to live playback — that latency lands directly in
the live-session buffer budget.

The helper side already does it right: a dedicated thread blocked in `ReadFile`
signalling an event (`ResidentWorkerLoop.h:235`). **The cheap direction got the
good mechanism.**

**Three smaller items in the same path:**

- `NeuralWorkerProtocol.h:225` — **two** `WriteFile` syscalls per message
  (header, then payload) under a mutex on the render thread, once per encoded
  frame. 120 syscalls/s to move 3.8 KB.
- `NeuralWorker.cpp:426` — the **60 Hz metadata pipe takes the default 4 KiB**
  while the low-traffic command pipe gets an explicit 16 KiB (`:436`). Backwards.
- `NeuralWorker.cpp:143` — `std::array<std::byte, 4096> chunk{};`
  value-initialized **inside** the drain loop. A 4 KiB memset per poll to
  receive ~64 bytes.

**Fix** — mirror the helper: a parent-side reader thread blocked in `ReadFile`.
Or switch to `CreateNamedPipe` + `FILE_FLAG_OVERLAPPED` and use a real
`WaitForMultipleObjects` — which is what the `ResidentWorkerLoop.h:63` comment
says was unavailable. Coalesce header+payload (every message but
`Result`/`Preflight` is ≤92 bytes). Give the metadata pipe 64 KiB.

---

### [ ] 2.9 · Cache hashes everything, every time

🔍 reported · **effort: M** · **gain: 3-5 s per job start on a 5 GB source**

**Where** — `src/NeuralCache.cpp:519`, `:982`, `src/main.cpp:5686`

- `Lookup` full-hashes the payload on **every** call — with **no stop token**,
  so a multi-GB hash is uncancellable.
- Every `StartNeuralJob` hashes the whole source first — **including the
  `prepareOnly` cache check and every live-session retarget.**
- No `FILE_FLAG_SEQUENTIAL_SCAN`, 1 MiB buffered `ifstream`.

**Live-session write amplification: 2 full writes + 3 full reads per rendered
byte.** Segment files → `ConcatenateMedia` copies all into
`staging/neural.mkv` → `ProbeMedia` → `Sha256File` in `Promote` → post-rename
manifest re-read. **The segment files are never deleted** (`main.cpp:6946`), so
steady-state occupancy is ~2× the render output.

**Cost** — ~40-80 ms per lookup on a 60 MB render (the code's own comment
measures 63-86 ms); **3-5 s per job start on a 5 GB source**.

**Fix**

- [ ] Hash the source **once per loaded media**, carry the digest on the
      session
- [ ] Promote to `Sha256FileCached` for payloads this process published this
      session, keyed on `(path, size, mtime, promotionSequence)`. The
      correctness objection in `NeuralCache.h:104` is about *user* content — a
      just-promoted payload is not that.
- [ ] `FILE_FLAG_SEQUENTIAL_SCAN`, 4 MiB buffer
- [ ] Delete the joined `staging/neural.mkv` copy, or delete the segments after
      the join

---

### [ ] 2.10 · Smaller, cheap, worth doing

🔍 reported · **effort: XS each**

| Item | Where | Why |
| --- | --- | --- |
| `Log::Write` holds a global mutex across `OutputDebugStringA` **and** a flushed file write | `Log.h:14` | `OutputDebugStringA` takes the system-wide `DBWinMutex`. Per-reset logging (`D3D12Renderer.cpp:1020`) becomes per-frame in a degraded session. Buffer + flush on a timer; skip unless `IsDebuggerPresent()` |
| Capture fence wait is on the **render** thread | `OfflineNeuralRenderer.cpp:2020` | Only the memcpy is offloaded; any GPU hiccup lands on the loop. Move `BeginResolveOldestCapture` into the `DeferredCapture` worker |
| Recycle pool is 4 buffers against a ~30-frame queue | `:444` vs `:318` | A miss makes `CopyCaptureView`'s `resize` zero-fill a whole frame first — **33 MB memset at 4K.** Size the pool to the queue depth, as the single-file path already does (`:2189`) |
| First-frame receipt gate polls a log file at 100 ms, up to 2 s, needing 3 stable samples | `:1379` → `:2264` | Seconds on frame 0 of a cold job, re-reading up to 4 MiB with a `LowerAscii` copy each time. Use `ReadDirectoryChangesW` or a tail read from the last offset |
| `ChildProcess::Wait` polls at 25 ms | `MediaPipeline.cpp:223` | Up to 25 ms per segment publish. `WaitForSingleObject(process, INFINITE)` with the existing deadline as timeout |
| Telemetry vectors: 10 `push_back`/frame, no `reserve` | `OfflineNeuralRenderer.cpp:116`, `:1220` | ~7 MB at 100k frames + realloc-and-copy on the render thread |
| `SelectSegment` returns `NeuralSegment` **by value** per decoded pair | `SynchronizedPlayback.cpp:429` | A `std::filesystem::path` wide-string allocation per frame **under the index mutex**. Return the index or a `string_view` |

---

## 2B · Build system

---

### [x] 2.11 · A reachable path to a fully unoptimized shipping binary

🔍 reported · **effort: S** · **impact: measurement integrity**

`CMakeLists.txt:184` and `:206` are the **complete** compile-option set for
both shipped binaries:

```cmake
target_compile_options(DLSSVideoPlayer PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
```

Zero hits across the file for `/O2`, `/GL`, `/LTCG`, `/Gy`, `/OPT:ICF`,
`/arch:`, `/fp:`, `/MP`, `/GENPROFILE`, `target_precompile_headers`,
`UNITY_BUILD`, `CMAKE_INTERPROCEDURAL_OPTIMIZATION`.

**`CMAKE_BUILD_TYPE` is never set, defaulted, or validated.** And
`build_windows.bat:102` has a branch that passes **no generator**:

```bat
) else (
  echo [4/5] Configuring with CMake's default generator...
  "%CMAKE_EXE%" -S . -B build-upscaling -DBUILD_TESTING=ON ...
)
```

This fires when `cmake.exe` came from `PATH` (`:30`). If that CMake defaults to
Ninja or NMake:

- `CMAKE_BUILD_TYPE` is empty → **no `/O2`, no `/Ob2`, no `NDEBUG`** (asserts live)
- `--build --config Release` at `:117` is **silently ignored** by single-config
  generators
- The script then prints `[OK] ...\Release\DLSSVideoPlayer.exe` **without
  checking the path exists**

CI is safe (both workflows pin `-G "Visual Studio 17 2022"`). This only bites
local builds — and anything packaged from one. **For a project that measures
this carefully, that is a correctness-of-measurement hazard.**

**Fix, in order**

- [ ] `if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
      set(CMAKE_BUILD_TYPE Release) endif()`
- [ ] `Test-Path` check on the output in `build_windows.bat`
- [ ] `add_compile_options(/MP)`
- [ ] `set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)` behind
      `check_ipo_supported` — LTCG inlines across `NeuralCache.cpp` ↔ `main.cpp`
      and `SynchronizedPlayback.cpp` ↔ `VideoDecoder.cpp`; typically 2-5%
- [ ] `/Gy` + `/OPT:ICF`

> ⚠️ **Leave `/arch:AVX2` and `/fp:fast` alone.** The first excludes pre-Haswell
> CPUs. The second would perturb the bit-identical-render property the
> benchmark relies on.

---

### [~] 2.12 · 5.2× compile amplification — there is no `add_library` anywhere

🔍 reported · **effort: M** · **gain: ~halve a 10-minute CI**

`grep -c add_library CMakeLists.txt` → **0**. 20 `add_executable`.

Measured in the existing build tree: **162 `.obj` files for 31 `.cpp` sources,
76.7 MB.**

| Source | Compiled |
| --- | ---: |
| `VideoDecoder.cpp` (1,661 lines) | **9×** |
| `NeuralCache.cpp` | 8× |
| `NeuralPreflight.cpp` | 7× |
| `MediaPipeline.cpp` | 6× |
| `D3D12Renderer.cpp` | 5× |

Roughly 104k TU-lines against 27.8k lines of source — an undercount, since
`PlayerUiRegressionTests` pulls the whole player list again via
`get_target_property` at `:641`. Recent CI runs: **10m04s and 10m23s**.

**Verified safe** — only 5 files reference a per-target define:

- `DLSS_VIDEO_PLAYER_VERSION` → `main.cpp`, `NeuralPreflightProbe.cpp`,
  `UpdateCheck.cpp`, `resources.rc`
- `DLSS_VIDEO_PLAYER_HAS_NVOF` → `OpticalFlowNvof.cpp` only
- `*_TESTING` → `main.cpp`, `YouTubeResolver.cpp`

No source references `__cplusplus`, so the `/Zc:__cplusplus` inconsistency at
`:274`/`:376` is harmless.

**Fix** — three `OBJECT` libraries:

- [ ] `player_media` — VideoDecoder, MediaSource, MediaPipeline,
      SynchronizedPlayback, AudioPlayer
- [ ] `player_render` — D3D12Renderer, OpticalFlowNvof, DLSSBackend,
      DLSSGBackend, TemporalGuides, RuntimePolicy
- [ ] `player_neural` — NeuralCache, NeuralPreflight, NeuralWorker,
      NeuralReceipt, OfflineNeuralRenderer, RuntimeLock, ReShadeConfig,
      NeuralSettings

---

## 2C · Tests and CI

---

### [ ] 2.13 · Zero A/V sync coverage

🔍 reported · **effort: M** · **impact: the most user-visible property**

`SynchronizedPlayback` is **not** A/V sync — it pairs the *original* and
*neural* video streams.

`AudioPlayer` is tested only for process/pipe lifetime
(`PolicyTests.cpp:5547`). The sole timing assertion is:

```cpp
CHECK_EQ(7.5, AudioPlayerTestAccess::SeekBase(*audio))
```

— that a value was **stored**. Never correlated to a video PTS. No drift
measurement, no clock-master test, no resync-after-seek assertion.
`CONTRIBUTING.md:15` asks contributors to test this **by hand**.

**Why the arithmetic is unforgiving** — drift = **7.2 ms per ppm** over a
2-hour film. A typical 50 ppm consumer crystal drifts **360 ms**, past ITU-R
BT.1359-1's acceptability window (+90/-185 ms).

**Fix** — generate a clip with a 1 kHz tone burst on the frame where a marker
appears (FFmpeg `lavfi`, the same way `CMakeLists.txt:403` already generates the
phase-probe clip). Assert `|AudioPlayer position − video PTS| < one frame` at:

- [ ] start
- [ ] after a forward seek
- [ ] after a backward seek
- [ ] after pause/resume

Add it to `CachedExportTests`, which already has real media staged.

---

### [x] 2.14 · 8 GPU tests never run anywhere, and hard-fail instead of skipping

🔍 reported · **effort: S for the skip, M for the runner**

`CMakeLists.txt` registers 21 tests: 13 portable, **8 labelled `gpu`** (`:420`,
`:450`, `:515`, `:518`). CI runs `ctest -LE gpu` (`build.yml:67`,
`release.yml:77`). **No self-hosted or GPU runner exists.**

Two modules live almost entirely behind that label:

- `src/DLSSGBackend.cpp` — 27 KB
- `src/FrameGenerationPass.cpp` — 49 KB

**76 KB with no CI-executed assertion, in the newest and most complex feature.**

Worse: `tests/DlssgProbeSmoke.cpp:104` prints `probe=unreachable` and
`return 2` — but `SKIP_RETURN_CODE` is set on **exactly one test** in the whole
file (`CMakeLists.txt:287`, `CachedExportTests`). On a GPU-less machine
`ctest -L gpu` reports **hard failures, not skips**.

**Mitigating** — the manual substitute is unusually rigorous: 7 dated
`docs/VERIFICATION-*.md` records, 11 measurement sets, and a 77 KB
`tools/verification/player_session.ps1` that drives the real player via
`SendInput` and parses the log. **The automation already exists; only the
runner is missing.**

**Fix**

- [ ] Give the 5 GPU smokes `SKIP_RETURN_CODE 2` — do this first, it is minutes
- [ ] Self-hosted runner on the RTX 4080 SUPER, `schedule:` +
      `workflow_dispatch`, running `ctest -L gpu`
- [ ] Fix `docs/BUILDING.md:45` — it tells contributors to run `ctest` with
      **no `-LE gpu`**, contradicting `:72` on the same page

---

### [ ] 2.15 · Nothing structurally guards cache-key completeness

🔍 reported · **effort: S** · **impact: the failure the whole receipt
architecture exists to prevent**

`BuildNeuralCacheKey` (`NeuralCache.cpp:581`) is a **hand-maintained field
list**. Tests discriminate 11 fields individually
(`NeuralPrerenderTests.cpp:291`, `RenderSettingsTests.cpp:212`) — genuinely
good — but nothing is derived from the struct.

**Adding a 14th field to `NeuralCacheIdentity` compiles and passes green.**

And the gap is already live: `NeuralPrerenderTests.cpp:267` sets
`identity.height = 1080`, but the mutation loop varies only
`changed.width = 2560`. **`height` is never varied.**

**Fix**

- [ ] Add a canary near `BuildNeuralCacheKey:581`:

```cpp
static_assert(sizeof(NeuralCacheIdentity) == /*pinned*/,
    "NeuralCacheIdentity changed: add the field to BuildNeuralCacheKey and to "
    "published_render_is_not_reused_across_identity_changes_test.");
```

- [ ] Vary `identity.height` in the mutation loop at
      `NeuralPrerenderTests.cpp:288`

---

### [ ] 2.16 · One 1,894-line test function with no crash guard

🔍 reported · **effort: M**

`tests/PlayerUiRegressionTests.cpp:118-2012` is a **single**
`PlayerAppTestAccess::Run()` holding all **485 assertions**, called once from
`main()`. No case names, no isolation, no SEH guard.

`PolicyTests.cpp:7130` already solved exactly this and it was never backported:

```cpp
void run_case_guarded(void (*run)(), CaseOutcome& outcome) noexcept
{ __try { run_case_catching(run, outcome); }
  __except (EXCEPTION_EXECUTE_HANDLER) { outcome.exceptionCode = GetExceptionCode(); } }
```

with the comment at `:7127`: *"An access violation or a stack overflow in one
case used to take every case after it, and the failing name, with it."*

**Related harness issue** — `CHECK` is **non-fatal** (`TestSupport.h:21` only
increments a counter), which forces **~218** `CHECK(x.has_value()); if (!x) return;`
pairs across the suite. Each one silently converts a failure into *abandoning
the rest of the case*. There is no `REQUIRE`.

**Fix**

- [ ] Lift `TestCase` / `TEST_CASE` / `run_case_guarded` from
      `PolicyTests.cpp:7104` into `tests/TestSupport.h`
- [ ] Split `Run()` into named cases
- [ ] Add a fatal `REQUIRE` macro

---

### [ ] 2.17 · Missing CI guardrails

🔍 reported · **effort: M**

Verified absent: no sanitizers (`/fsanitize=address`), no static analysis (no
`/analyze`, no `.clang-tidy`, no `.clang-format`, no CodeQL), no
warnings-as-errors (`/W4` on all 20 targets, never `/WX`), **no Dependabot** (so
SHA-pinned actions have no refresh path), no `timeout-minutes`, no
`CMakePresets.json`, no `CMAKE_EXPORT_COMPILE_COMMANDS`.

For **33 mutexes / 36 `jthread` / 17 atomics / 13 condition variables** across
23 files, and **zero `assert()` in all of `src/`**, that is a thin net.

**ASan is viable today** — `PolicyTests`, `NeuralPrerenderTests`, and
`PlayerUiRegressionTests` create **no D3D12 device** (verified: no
`D3D12CreateDevice` in any of them).

**Also** — `tools/fetch_youtube_helpers.ps1` carries four `# TEST-SEAM:` markers
(`:90`, `:155`, `:164`, `:181`) around its transactional backup/restore swap,
and **no harness uses them**. No Pester anywhere. The most intricate failure
path in `tools/` (`:159`, restore-failure preserving the backup) is untested.
`stage_runtime.ps1` and the non-`-PublicCore` path of `verify_package.ps1` are
likewise never exercised.

**Fix** — a separate quality job so it cannot slow the main path:

- [ ] `/fsanitize=address` on the three device-free suites
- [ ] `/analyze` on `DLSSVideoPlayer` + `NeuralWorker`
- [ ] `.github/dependabot.yml` for `github-actions`
- [ ] `timeout-minutes: 30` on every job
- [ ] Defer `/WX` until the current `/W4` count is measured —
      `CONTRIBUTING.md:11`'s *"when possible"* suggests it is non-zero

---

## 2D · Correctness follow-ups

---

### [ ] 2.18 · Renderer recovery cannot work — it reuses the HWND

🔍 reported · **effort: S** · **impact: the recovery path is dead on arrival**

`D3D12Renderer.cpp:122` deliberately **leaks** the renderer when the drain does
not complete:

```cpp
if(D3D12Renderer::s_retainedRenderers.fetch_add(1)==0){
    LOG("Renderer retirement retained after bounded GPU drain failure.");
    return;
}
```

`DrainForRetirement` short-circuits on an already-latched failure (`:1529`),
returning `TimedOut`/`WaitFailed`/`EventRegistrationFailed` — none of which is
`Completed` or `DeviceRemoved`. **So the old swapchain stays alive.**

`main.cpp:3835` then rebuilds with the **same** `m_renderWnd`, and
`D3D12Renderer.cpp:232` calls `CreateSwapChainForHwnd`. DXGI allows **one
flip-model swapchain per HWND** → `DXGI_ERROR_INVALID_CALL` → `Unload()` +
*"the GPU has been lost"* dialog.

**Every other candidate-renderer path** (`EnableUpscaling`,
`CreateRendererCandidate`) correctly creates a fresh child window. **Only the
recovery path reuses.**

**Repro** — a fence wait fails without device removal (e.g.
`SetEventOnCompletion` → `E_OUTOFMEMORY`, or a >20 s stall on a TDR-extended
machine). Media unloads and a whole D3D12 device + swapchain leaks.

**Fix** — create a fresh child render window in `RecoverUnusableRenderer`,
mirroring `EnableUpscaling`. Destroy the old one only after a retained renderer
is confirmed dead.

---

### [ ] 2.19 · No cache eviction at all

🔍 reported · **effort: M** · **impact: unbounded disk**

`RemoveSource` / `RemoveRender` exist (`NeuralCache.h:203`) but have **no
production callers** — only `PlayerUiRegressionTests.cpp` and
`RenderSettingsTests.cpp`. The only production reclamation is `Clear()`
(`main.cpp:6448`), which is all-or-nothing.

Meanwhile the key **deliberately retires entries wholesale**:
`applicationVersion`, `driverVersion`, `modelStoreDigest`, `runtimeDigest` and
the manifest schema are all key terms (`NeuralCache.cpp:581`). `Promote` only
reclaims a directory whose key is *re-rendered* (`:1079`).

**Repro** — a user with 40 GB of renders takes an NVIDIA driver update. Every
key changes. **All 40 GB becomes unreachable dead weight**, everything
re-renders, disk grows to 80 GB. The only remedy offered destroys the new
renders too.

`docs/ARCHITECTURE.md:444` still claims displaced keys are removed.

**Fix**

- [ ] On startup, enumerate `renders/` and delete entries whose manifest schema
      / application / driver / model terms no longer match
- [ ] Or a size cap with LRU eviction, guarded against active jobs
- [ ] Correct `ARCHITECTURE.md:444`

**Related smaller cache bugs:**

| Item | Where | Note |
| --- | --- | --- |
| `Clear()` skips `live/` but `SizeBytes()` counts it | `NeuralCache.cpp:1187` | Exactly the bug the comment two lines above says was fixed for `frame-generation` — *"Leaving it out made the Clear prompt lie"* |
| `SizeBytes()` returns 0 for the whole cache after one unreadable entry | `:1174` | Shared, never-cleared `error_code`. "Delete 0 MiB" for a 40 GB cache |
| `SweepStaging`'s 100 ms budget is checked only *between* candidates | `:876` | A single `remove_all` of a 30 GB abandoned staging dir is uninterruptible — **and it runs on the UI thread**, from 6 call sites |
| No `FlushFileBuffers` before the publishing rename | `:1050`, `:416` | `MOVEFILE_WRITE_THROUGH` flushes the *rename*, not contents. Detected on read (payload is hashed), but the dead entry is never reclaimed |
| Quarantine has no forensic window | `:866` | `invalid*` names are reaped by the very next manager construction, seconds later |

---

### [ ] 2.20 · Two `MAX_PATH` truncation bugs, and 13 copies of one function

🔍 reported · **effort: S**

`DLSSBackend.cpp:24` and `DLSSGBackend.cpp:61` are **byte-identical** and both
wrong:

```cpp
wchar_t exePath[MAX_PATH]{};
GetModuleFileNameW(nullptr, exePath, MAX_PATH);   // no return check
std::filesystem::path logDir =
    std::filesystem::path(exePath).parent_path() / L"ngx_logs";
```

On a path over 260 chars this truncates silently and creates `ngx_logs`
somewhere wrong.

`NeuralCache.cpp:793` **already does it correctly** with a 32768 buffer and a
length check. The codebase knows the right pattern; these two predate it.

**13 `GetModuleFileNameW` sites total, 3 incompatible buffer strategies.** Three
of them (`MediaPipeline.cpp:55`, `NeuralWorkerMain.cpp:31`,
`OfflineNeuralRenderer.cpp:2197`) are literally the same 6 lines with `!length`
vs `length == 0` as the only difference.

**Fix** — add `src/PlatformPaths.h` with one checked `ModuleDirectory()`, and
convert all 13. Start with the two truncation bugs.

---

### [ ] 2.21 · Other verified duplication worth collapsing

🔍 reported · **effort: M** · **impact: divergence, not compile time**

These matter because **the copies behave differently**, not because they are
repeated.

| Duplicate | Copies | The divergence that bites |
| --- | ---: | --- |
| **Wide↔narrow converters** | 18 | 3 incompatible ASCII semantics. `NeuralWorker.cpp:109` **rejects** a non-ASCII string; `NeuralWorkerMain.cpp:149` substitutes `'?'`. **These two are the parent and child of the same IPC channel.** |
| **`JsonEscape`** | 2 | `NeuralPreflight.cpp:469` emits `\u00XX` for a control char; `NeuralCache.cpp:117` **returns an empty string**. Same byte, two outcomes. |
| **Hex / NGX error formatting** | 1 canonical + 24 ad-hoc | `NeuralPreflight.cpp:522` emits `0xbad00002`; the 24 `<<std::hex<<` sites drop leading zeros, so `0x00000015` logs as `0x15` and **cannot be grepped against the receipt**. The header comment at `NeuralPreflight.h:113` claims it is the only place formatting lives — it is not. |
| **`CreateKillOnCloseJob`** | 6 | `MediaPipeline.cpp:114` and `NeuralWorker.cpp:74` are character-for-character identical |
| **`QuoteArgument`** | 5 | 2 correct (byte-identical), 2 naive `L"\"" + s + L"\""` applied to **resolved YouTube URLs**. Safe *only* because `YouTubeResolver.cpp:445` rejects `"`, `\`, `'` and whitespace first — **that coupling is undocumented at the ffmpeg call sites** |
| **Atomic file write** | 5 | Only `RecentMedia.cpp:174` and `ReShadeConfig.cpp:556` actually `FlushFileBuffers`. `NeuralWorker.cpp:1613` writes the live receipt path with a plain truncating `ofstream` — no temp, no rename |
| **Full `CreateProcess` launch sequence** | 7 | — |
| **Pipe drain loop** | 6 | — |
| **Hex nibble table** `"0123456789abcdef"` | 5 | — |
| **`WriteMessage` / `WriteCommand`** | 2 | `NeuralWorkerProtocol.h:222` and `:235` have **identical bodies three lines apart**; only the enum parameter type differs |

**Also worth noting:** `AudioPlayer.cpp:34`'s helper lookup lacks the
`neural-runtime` guard that `MediaPipeline.cpp:65` and `VideoDecoder.cpp:302`
both have, then falls back to `SearchPathW` — **it will pick an arbitrary PATH
ffmpeg that the other two deliberately refuse.**

---

### [ ] 2.22 · Documentation drift

🔍 reported · **effort: XS**

| Doc | Says | Reality |
| --- | --- | --- |
| `packaging/runtime-lock.json` | 10 of 12 `provenance` strings say *"user-supplied matching pack"* | `fetch_neural_runtime.ps1:36`, `:61` have real URLs + digests. `SECURITY.md:16` and `THIRD_PARTY.md:7` point auditors **at the stale copy**. `docs/BUILDING.md:123` has the correct table |
| `docs/BUILDING.md:70`, `CONTRIBUTING.md:23` | *"the two `gpu`-labelled smokes"* | There are **8** |
| `docs/BUILDING.md:45` | `ctest ... --output-on-failure` | Missing `-LE gpu`; hard-fails on a GPU-less box, contradicting `:72` |
| `docs/ARCHITECTURE.md:385` | omits `range` and `guides` | Both **are** keyed |
| `docs/ARCHITECTURE.md:444` | claims displaced keys are removed | They are not — see **2.19** |
| `docs/ARCHITECTURE.md` | says offline uses software decode | Offline uses `-hwaccel cuda` too (`VideoDecoder.cpp:238`) |
| `docs/RELATED_PROJECTS.md` | reviewed 2026-09-01 | **Four weeks and nine competitor releases stale** — see **3.1** |

---

### [ ] 2.23 · Smaller confirmed items

🔍 reported · **effort: XS each**

- **`WM_DROPFILES` discards a multi-file drop silently** — `main.cpp:6701`
  computes `count`, checks it, then ignores it. Also a 64 KB stack buffer
  inside `WndProc`.
- **Zero coverage on two shipped files** — `NeuralWorkerMain.cpp` (633 lines,
  the helper's real `wmain`) and `NeuralPreflightProbe.cpp` (163 lines) are in
  **no** test target. `ParallelFor.h` is listed in `PolicyTests`' sources but
  never included or called.
- **`harness_sanity_test`** (`PolicyTests.cpp:2066`) is `CHECK(true); CHECK_EQ(2+2, 4);`
  registered as a real case and counted in the pass line.
- **Unbounded on-disk values** — `VideoDecoder.cpp:482` parses `width`/`height`
  with no ceiling anywhere (a crafted MKV declaring 20000×20000 asks for ~11 GB);
  `:500` checks `duration` only for `isfinite && > 0`, unlike the Matroska path
  at `:148` which correctly bounds against `INT64_MAX/1e7`.
- **`OpenKnown` bypasses the fps clamp its own header documents** —
  `VideoDecoder.cpp:881` assigns `fps` directly; the `std::clamp(fps, 1.0, 240.0)`
  lives in `ProbeFFmpeg` (`:584`), which a known open skips.
  `VideoDecoder.h:216` states the opposite invariant.
- **`MetadataReader::ReadAvailable` is an unbounded drain loop** —
  `NeuralWorker.cpp:132`, no iteration cap, byte cap or time budget. It is the
  first statement of every `Pump` iteration, **before** the stop check. A chatty
  helper makes the render uncancellable.
- **`EndHelper` does an unbounded blocking `WriteFile` before any kill** —
  `NeuralWorker.cpp:382`, on a synchronous anonymous pipe, before
  `TerminateJobObject`. Once ~16 KiB accumulates the job thread hangs at shutdown.
- **Exit-code misclassification** — `NeuralWorker.cpp:577` calls
  `GetExitCodeProcess` on a **still-running** helper and reports *"exited with
  code 259"*. `:1402` ignores the return value, turning a genuine crash into
  non-relaunchable `Protocol`.
- **A read-only install hard-fails YouTube as "helper files are missing"** —
  `YouTubeResolver.cpp:110` tolerates only `ERROR_ALREADY_EXISTS`;
  `ERROR_ACCESS_DENIED` propagates. Five distinct causes (**including detecting
  a junction attack**) share one opaque string, with no log line and no
  LocalAppData fallback.

---
---

# Tier 3 — Strategic

Features, positioning, and the subsystems that need real design.

---

### [ ] 3.1 · Update the competitive framing — do this first, it costs an hour

`POSITIONING` · ✅ verified via GitHub API · **effort: XS** · **impact: credibility**

`docs/RELATED_PROJECTS.md` treats **Merserk/dlss5-visual-enhancer** as an
integration reference reviewed 2026-09-01. That framing is now wrong.

| | this project | Merserk |
| --- | ---: | ---: |
| Stars / forks | 131 / 12 | **942 / 75** |
| Release downloads | 5,492 / 17 releases | **42,154 / 12 releases in 18 days** |
| Cadence | ~0.5/week | v0.1 → v10.0 in 18 days |
| Stack | C++20 / D3D12 / Win32 | Python + QML + embedded mpv |
| License | MIT | custom, `NOASSERTION` |

Plus [NeuralScreen](https://github.com/perseval-BLR/NeuralScreen) (891★, DLSS 5
NR on the whole desktop), [video2dlssnr](https://github.com/DaniilSokolyuk/video2dlssnr)
(168★), a DaVinci OFX plugin, an OBS filter, 20+ ComfyUI packs.

**`README.md`'s "neighboring products do offline upscaling with a progress bar"
is no longer true.**

**But the moat is real — it is just narrower than the current claim.** Merserk's
Live mode is `mpv_embed.py` + a loopback HLS server with `Cache-Control:
no-store`, a 720p default input cap, no cross-session cache, and no
original/neural pairing during Live. Its Split/2-Up viewer compares finished
*files* in the batch workflow.

**Genuinely unique, verified:**

1. **Progressive whole-video coverage with seek-anywhere, nothing discarded.**
   `NeuralCoverage.h`'s span algebra has no counterpart anywhere.
2. **A persistent, settings-hashed, validated render cache** reused across
   sessions.
3. **Frame-accurate split/wipe/blend comparison *while a live session runs*.**
   `main.cpp:5123` sets `m_cachedPlayback=true` on live attach. mpv's
   side-by-side request [#3854](https://github.com/mpv-player/mpv/issues/3854)
   has been open for years; madVR offers only profile-toggle hotkeys;
   VLC/PotPlayer have nothing; Topaz compares two *finished* renders.
4. **Render receipts and a published validation contract.** Merserk's tracker
   contains a literal request for source verification because it cannot offer
   this.
5. **The measurement culture as product** — `tools/benchmark/` has no equivalent,
   commercial or free.
6. **MIT + provenance** against a closed-source competitor with an
   antivirus-flagged bundled DLL.

**Already commoditized — stop leading with these:** "AI upscaling while you
watch" (RTX VSR ships in VLC, PotPlayer, mpv 0.39+, Chrome, Edge, Firefox
126+); "DLSS 5 on video"; before/after comparison as a *concept*; 2×-5×
interpolation (SVP does it **live** for $25 into five players).

**Fix**

- [ ] Rewrite `docs/RELATED_PROJECTS.md` against today's field
- [ ] Narrow the README claim to: *the only one that renders the whole video
      progressively, keeps every frame it renders, and puts the original and
      the render on the same frame at the same moment while it is still running*
- [ ] Lead with verifiability — MIT, reproducible build, hash-locked runtime,
      published receipts, seven dated hardware records. Currently buried in
      `docs/`.
- [ ] **Stop implying frame generation is part of the live story.** It writes a
      whole new file, competing against SVP's real-time switch. Your
      cadence-aware multiple selection is the better *engineering* — say that
      instead.

---

### [x] 3.2 · Named presets — the quality ladder lives here

`FEATURE` · **effort: XS** · **impact: highest polish-per-hour on the list**

**Depends on 1.1.**

You expose six model controls with measured tooltips. Excellent for an expert,
paralysing on a first run. Your own issue
[#1](https://github.com/2600th/dlss5-video-player/issues/1) is literally *"So
what resolution do I have to give to this?"*

Merserk ships three NR Styles plus a Detail-Only preset. Topaz surfaces
resolution-based recommendations in a carousel.

**This is where the quality rule gets implemented.**

- [ ] Ship named presets (e.g. Detail-Only / Natural / Cinematic)
- [ ] **Default to the near-best rung, and label it as the recommended one**
- [ ] Print the measured cost beside every rung — you already have the numbers.
      The existing NVENC tooltip is the model:

  > *"Measured at 2560x1440 on an RTX 5090: p7 takes twice the encode time of
  > p5 and buys 0.12 VMAF on ordinary content, 0.53 on noise-heavy content, at
  > 95-98 VMAF."*

- [ ] Never move a default down the ladder to buy speed

---

### [ ] 3.3 · Spatial masking and protection compositing

`FEATURE` · **effort: S-M** · **impact: most-requested unmet feature in the category**

Per-region control of how much neural output is applied: a loadable mask image,
a feather radius, face/skin protection, and a detail-only mode that keeps source
colour and tone while keeping structural detail.

**Who has it** — Merserk only (Custom NR Mask, Mask Feather 0-128px, Face/Skin
Protection 0-1, Tone Preservation 0-1). **Nobody else in the entire category.**

**Demand evidence**

- Topaz: [*"Masking areas for less enhancement… would be a killer super nice new
  feature"*](https://community.topazlabs.com/t/masking-areas-for-less-enhancement-and-masking-areas-for-more-enhancement-would-be-a-nice-feature/53511)
  — Oct 2023, still open
- SVP: [subtitles visibly *"vibrating/shaking"*](https://www.svp-team.com/forum/viewtopic.php?id=7240),
  no fix offered
- Doom9: [*"when motion estimation fails, it fails catastrophically"*](https://forum.doom9.org/showthread.php?t=174410)
  — naming hardcoded subtitles and credits

**Why it is cheap for you** — Neural strength (0-200%) is **already** a
per-frame blend between the neural and original members in the presentation
shader. Making that weight spatially varying is **one extra texture and a lerp**.

**And it sidesteps a known dead end** — `docs/ARCHITECTURE.md:772` records that
NGX mask inputs are inert. You do this in *your* compositor, not in the model.

- [ ] Ship the manual mask + feather first
- [ ] Face detection is a separate dependency — defer it

---

### [ ] 3.4 · RTX Video Super Resolution as a second, comparable engine

`FEATURE` · **effort: M** · **impact: makes your comparison surface unmatchable**

Expose NVIDIA's *video*-trained model alongside DLSS 5 NR, including at 1×
(enhance without enlarging).

**Two reasons, and the second is the real one:**

1. DLSS SR is trained on **rendered game frames**. RTX VSR is trained on
   **compressed video** and does artifact reduction as part of upscaling — the
   correct model for your dominant source. Your primary acquisition path is
   YouTube, and 0.21.0 exists because you were getting 3899 kbps trailers.
2. **It feeds your one genuine differentiator.** Original vs DLSS 5 NR vs RTX
   VSR, on one frame, at one timestamp, during a live render. **Nothing on earth
   can currently show that — including Merserk.**

**API** — the [RTX Video SDK](https://developer.nvidia.com/blog/enhancing-low-resolution-sdr-video-with-the-nvidia-rtx-video-sdk/)
supports **D3D11, D3D12 and Vulkan** on Windows. You already own the D3D12
resources. NGX also carries `NVSDK_NGX_Feature_VideoSuperResolution`, but that
path is CUDA-only per the [NGX programming guide](https://docs.nvidia.com/rtx/ngx/programming-guide/index.html).
NVIDIA has [confirmed there is no NVAPI toggle](https://forums.developer.nvidia.com/t/implement-feature-to-switch-nvidia-video-super-resolution-via-nvapi/289524)
— the SDK is the only sanctioned route.

`docs/ARCHITECTURE.md:773` already lists "RTX Video modes" as remaining work.

**Bonus** — this also delivers a compression-artifact pre-pass for free (RTX VSR
at 1×), which otherwise needs a hand-written deband/deblock compute shader.

---

### [ ] 3.5 · Processing scale — with the quality rule attached

`FEATURE` · **effort: S-M** · **impact: converts "cannot keep up" into working sessions**

**Depends on 1.1** (it must be a cache-key term).

A selector for what resolution the model runs at, independent of output.

**Why** — it directly attacks your #1 published limit. Your own numbers: 4K30
heavy re-encode measured 24 fps and *"could not keep up"*; 1440p59.94 runs at
0.814× real time. Running NR at 75% and letting DLSS SR carry the rest converts
several refusals into sessions that work.

Merserk offers 25-200%. The [DLSS5-Autopilot](https://github.com/Kizzuwatnaa/DLSS5-Autopilot)
"Neural Upstream" route exists in the game-mod world for the same reason —
vanilla NR runs at *output* resolution and costs ~50% FPS.

> ⚠️ **Per the quality rule: the default is Source / 100%.** Sub-100% is an
> explicit, labelled rung with its measured cost printed. Above-100% is offered
> for people who want to spend more time for more quality.

---

### [ ] 3.6 · Temporal stability control

`FEATURE` · **effort: S-M** · **impact: the defining failure mode of neural video**

A dial that blends the current neural output against the previous one, gated by
your existing cut detector.

**Who has it** — Merserk (Shimmer Suppression 0.00-1.00, default 0.70). **Topaz
staff confirmed on the record there is [no deflicker tool](https://community.topazlabs.com/t/is-there-a-way-to-deflicker-videos-in-topaz-video-ai/89338)**
and suggested users run frame interpolation to mask it.

**Why it is cheap for you** — one history texture, one lerp, guarded by
`ClassifySceneCut` which already exists. You removed jitter in 0.20.0 and you
already **measure per-pixel temporal sigma** in `tools/benchmark/`.

**And you can tune the default against your own harness, which nobody else can.**

---

### [ ] 3.7 · Subtitles via libass — composited *after* the network

`BASELINE` · **effort: M-L** · **impact: highest-pain gap in the baseline survey**

No libass anywhere. Subtitles are only stream-copied during FG export
(`FrameGenerationPass.cpp:902`).

**A player that cannot show subtitles forces "export and watch elsewhere",
which structurally undercuts the watch-while-it-renders positioning.**

`docs/ARCHITECTURE.md:792` already names this as pending **with the correct
design**.

**The critical detail for this project** — composite subtitles **after** the
network. NVIDIA is explicit:

> *"For the best image quality, it is **critical** to provide a Hudless (pre-UI)
> buffer and a UI buffer… expect image quality degradation on those elements."*
> — [Streamline ProgrammingGuideDLSS_G.md §5.1](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)

Subtitles are HUD. Interpolated text warps, ghosts and shimmers — optical flow
has no correspondence for a caption that appears from nothing and is static
while the scene moves under it.

**Three implementation details routinely got wrong:**

- `ass_set_storage_size` is **mandatory**, not optional — the header says
  *"storage size must be configured to get correct results, otherwise libass is
  forced to make a fallible guess"*
- `ASS_FONTPROVIDER_DIRECTWRITE` is the Windows backend; embedded MKV attachment
  fonts need `ass_add_font` or typeset signs collapse
- Render at **output** resolution, not video resolution, or text is soft on 4K

**Baseline UX users treat as non-negotiable:** delay adjust (±0.1 s), track
switch mid-playback, external auto-load, encoding detection (uchardet), scale /
position.

**Must-have formats:** SRT, **ASS/SSA**, PGS, VobSub, WebVTT, `mov_text`. ASS is
not optional — a player that renders SRT but flattens ASS has, from the user's
perspective, no subtitle support for that content.

---

### [ ] 3.8 · WASAPI audio

`BASELINE` · **effort: L** · **impact: severe, affects everyone**

Current: `waveOut` (winmm) fed by an `ffmpeg.exe` subprocess
(`AudioPlayer.cpp:67`). No WASAPI, no device-change handling, no passthrough, no
drift correction.

- [ ] **Shared-mode WASAPI, event-driven, float32 at the mix format** as the
      default. Microsoft's 2026 guidance actively steers *away* from exclusive
      mode.
- [ ] **Device change** — register **both** `IMMNotificationClient`
      (`OnDefaultDeviceChanged`, `OnDeviceStateChanged`, and the forgotten
      `OnPropertyValueChanged` for `PKEY_AudioEngine_DeviceFormat`) **and**
      `IAudioSessionEvents::OnSessionDisconnected`. Add a watchdog — Kodi treats
      `WaitForSingleObject(needDataEvent, 1100) != WAIT_OBJECT_0` as a dead sink
      because some drivers stop signalling without erroring.
- [ ] **A/V sync** — audio-master, clocked from `IAudioClock::GetPosition` + QPC.
      Correct drift by resampling with `swr_set_compensation`, capped at mpv's
      0.125% default.
- [ ] **Multi-track** — never auto-select `AV_DISPOSITION_COMMENT`,
      `_VISUAL_IMPAIRED`, `_DESCRIPTIONS`, `_HEARING_IMPAIRED`. Auto-picking the
      director's commentary is a top-tier complaint.
- [ ] A 2-5 ms cosine fade on every seek/pause kills essentially all click
      complaints.
- [ ] Bitstream passthrough behind a toggle — use FFmpeg's `spdif` muxer, do not
      hand-roll MAT framing.

**Two known bugs worth designing against:** mpv
[#1773](https://github.com/mpv-player/mpv/issues/1773) — `IAudioClient::Release()`
**hangs indefinitely** after a format change during exclusive playback. Kodi
[#18453](https://github.com/xbmc/xbmc/issues/18453) — ending fullscreen playback
switches display refresh, dropping the HDMI audio sink → access violation.
**Directly applicable to a D3D12 player that changes display mode.**

---

### [ ] 3.9 · Detect VFR and refuse frame generation on it

`BASELINE` · **effort: S** · **impact: sharper for this product than for a normal player**

`r_frame_rate` and `avg_frame_rate` are both lies for screen recordings (OBS,
ShadowPlay, Game Bar), phone video, and mixed-telecine anime.

**Why this is worse for you than for other players** — interpolating between two
frames whose real spacing is 16 ms and then 83 ms, while assuming uniform
spacing, produces motion that **speeds up and lurches**.

**Fix** — detect VFR from PTS-delta variance over a window, and **refuse frame
generation with a stated reason** rather than producing wrong motion.

This fits what 0.24.0 already does: *"When it cannot, it says why instead of
greying out."* VFR is simply one more reason on that list.

---

### [ ] 3.10 · CLI / headless invocation

`FEATURE` · **effort: S** · **impact: cheapest route to batch**

`DLSSVideoPlayer.exe --input X --range A-B --settings Y --out Z`

- Cheapest possible route to the batch use case **without building queue UI**
- Makes your own benchmark harness a first-class consumer
- You already parse argv (`ParseRuntimeArguments`) for safe mode

video2x is CLI-first; Topaz exposes its `tvai_up`/`tvai_fi` command line, which
is what every third-party wrapper wraps.

---

### [ ] 3.11 · Surface quality metrics in-app

`FEATURE` · **effort: M** · **impact: a differentiator disguised as a gap**

Show flicker, temporal sigma, colour delta, and a blind A/B for the current
settings on the current clip.

**Nobody does this.** Topaz users cannot tell whether a setting helped, which is
why [three](https://community.topazlabs.com/t/good-way-to-compare-preview-results/39250)
[separate](https://community.topazlabs.com/t/preview-bulk-settings-and-comparison/44706)
[threads](https://community.topazlabs.com/t/can-i-do-a-side-by-side-display-of-original-and-processed-video-as-in-the-previde-mode/44059)
ask for better comparison.

**You already compute all of this in `tools/benchmark/`.** It fits
`PRODUCT.md`'s *"Numbers over adjectives"* better than anything else on this
list.

---

### [ ] 3.12 · Exposed scene-change controls and duplicate-frame handling

`FEATURE` · **effort: S for the slider, M for dedup**

0.24.0 added cut detection. Expose the threshold you already compute.

**The evidence that one fixed threshold satisfies nobody is unusually clean:**
SVP [#7411](https://www.svp-team.com/forum/viewtopic.php?id=7411) wants cut
detection **more** aggressive; [#6615](https://www.svp-team.com/forum/viewtopic.php?id=6615)
wants it **disabled entirely**. Same product, same year.

Separately, animation shot on twos/threes needs dedup — which is why
[ddfi-rife](https://github.com/Mr-Z-2697/ddfi-rife) and
[MultiPassDedup](https://github.com/routineLife1/MultiPassDedup) exist.

**Your advantage** — `cutlab.py --sweep` already scores cut precision/recall, so
you can ship a **defensible default**, which is itself a differentiator.

---

### [ ] 3.13 · HDR end to end

`BASELINE` · **effort: L** · **impact: highest cost, narrowest audience, most
technically distinctive**

**Structurally blocked today:** `DXGI_FORMAT_R8G8B8A8_UNORM` swapchain
(`D3D12Renderer.cpp:229`), no `SetColorSpace1`, and `colorBuffersHDR = false`
hardcoded at `DLSSGBackend.cpp:410` while
`NVSDK_NGX_DLSS_Feature_Flags_IsHDR` exists.

Your README already states the consequence plainly: *"The current cache is
8-bit; another container or bit-depth label cannot recover lost precision."*

**Four traps specific to neural processing — not hypothetical:**

1. **Auto-exposure over PQ is meaningless.** Without a tagged exposure buffer,
   DLSS auto-enables `AutoExposure`. Statistics over PQ code values are not
   luminance statistics — PQ puts 1000 nits at ~0.75 and 10000 at 1.0, so the
   network **under-weights exactly the region HDR exists to show**.
2. **Per-frame peak detection + frame generation = brightness flicker.**
   libplacebo ships `peak_smoothing_period` and `black_cutoff` precisely because
   naive peak detection shimmers. Generated frames beat against the detector.
3. **Cache poisoning.** Storing tone-mapped output bakes in a **display-dependent
   mapping**. The same cached segment is permanently wrong on a second monitor,
   or after the user changes SDR white level. → **Cache scene-referred, or key
   the cache on display parameters.**
4. **NVIDIA's own video path took multiple driver generations to reach HDR.**
   MPC-VR's changelog: *"'Super Resolution' will only be enabled for 8-bit video
   due to driver limitations"* → later *"now works with HDR passthrough.
   Requires GeForce driver 572 or newer."* Assume it is hard.

**Correct approach, briefly** — detect via `IDXGIOutput6::GetDesc1` (Microsoft
explicitly says **do not** use `GetContainingOutput`); use
`DXGI_FORMAT_R10G10B10A2_UNORM` + explicit
`SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)` — Microsoft names
the video-player case for this; match SDR reference white via
`DISPLAYCONFIG_SDR_WHITE_LEVEL` (libplacebo hardcodes `PL_COLOR_SDR_WHITE 203.0f`
per BT.2408); do **not** call `SetHDRMetaData` — its docs now carry a warning
banner recommending apps tone-map into the monitor's reported range instead.

> ⚠️ **Default to not touching the OS HDR toggle.** MPC-VR's hard-won setting is
> *"Windows HDR: Do not change"* — *"This will prevent unexpected screen
> flickering."*

**The opening** — Merserk's own issue #35 is HDR export dropping colour-space /
VUI / SEI metadata. **Your colour-probe discipline is exactly the culture that
gets this right when the leader gets it wrong.**

---

### [ ] 3.14 · Extract testable units from `main.cpp`

`MAINTAINABILITY` · **effort: M** · **impact: velocity**

**The diagnosis is not what you would expect.** `main.cpp` has **zero global
mutable state** — every file-scope `static` is `constexpr`/`const`, no anonymous
namespaces.

The barrier is a single god object: **`class PlayerApp` spans L1179-7008 = 5,830
lines (83%), ~265 methods, ~188 members.**

And 7,049 lines **understates** it: 660 lines exceed 120 chars, 207 exceed 200,
the longest is **1,731 chars**. `main.cpp:6825` declares 16 booleans on one
line; `:6864` declares 24 members. By contrast `NeuralWorker.cpp` has **zero**
lines over 120. Reformatted to the rest of the tree's style, main.cpp is
~11-12k lines.

**You already know how to do this.** `IFrameSource` / `INeuralFrameEvaluator` /
`IFrameEncoder` (`OfflineNeuralRenderer.h:142`) and `ISynchronizedFrameSource`
are exactly why `NeuralPrerenderTests` can be portable. Policy extraction is
already house style: `PlaybackTiming.h`, `FrameRatePolicy.h`,
`LiveSessionPolicy.h`, `UpscalingPolicy.h`, `AppMenu.h` — each with matching
tests.

**Start with three, following the existing `*Policy.h` pattern — not a new
abstraction layer:**

- [ ] `ClampSeek` (`main.cpp:3905`, 15 lines, documented past bugs, depends on 5
      scalars)
- [ ] `LoadRenderPace` / `SaveRenderPace` (`:2783`, a string format with a
      legacy fallback)
- [ ] `ParseArgs` (`:848`, the loop at `:856` is already pure)

Then decompose the two real outliers:

- [ ] `StartNeuralJob` (`main.cpp:5566-5923` — **358 lines, 7 parameters**)
- [ ] `RunJob` (`OfflineNeuralRenderer.cpp:866-1549` — **684 lines, 11
      parameters**, containing a **337-line `[&]` lambda** at `:1126`)

Everything else in the `Neural*` files is under 200 lines. There is **no
commented-out code and no `#if 0` anywhere in the tree.**

---
---

# Tier 4 — Parked

Researched, and deliberately **not** doing. Recorded so they do not get
re-proposed.

| Item | Why not |
| --- | --- |
| **Custom model loading (.pth/.onnx)** | Structurally impossible. You call NGX feature 18 with NVIDIA's fixed weights. chaiNNer / Spandrel / OpenModelDB are a different product category. Do not pretend otherwise. |
| **Watch folders** | Requested at Topaz since [May 2022](https://community.topazlabs.com/t/feature-request-watch-folder/31932), re-requested 2023/2024/2025, never shipped, no official reply. Four years of asking with no revealed urgency. |
| **Stabilization, deinterlacing, colorization, face restoration** | Each needs a model you cannot obtain. Building inferior versions dilutes a single-model product. |
| **8K/16K output, 480 fps frame generation** | Marketing checkboxes. No panel shows them. Your cadence-aware multiple selection is the **correct** design — say so rather than matching the number-go-up menu. |
| **Cloud rendering / credits / tiering** | Topaz's [subscription transition thread](https://community.topazlabs.com/t/topaz-studio-transition-questions/95039) runs 1,200+ replies of pure anger. Your MIT/free position is an asset against a 942★ competitor with a Patreon and a bespoke license. |
| **Linux / mobile ports, multi-GPU splitting of one render, "auto" model recommendation** | NGX feature 18 on Windows D3D12 is the whole product. Multi-GPU splitting is meaningless for a single NGX session (**separate AI-GPU and NVENC-GPU selection is the useful version** — you already have `GpuPreference.cpp`). Auto-recommendation needs ~25 models; you have one. |
| **Full zero-copy decode/encode rewrite** | ~6 full-frame copies/frame today; no `CreateSharedHandle` / `cuGraphics` anywhere. Worth maybe 3-4 ms more — but it is a rewrite of both boundaries, and the ffmpeg-child architecture is load-bearing for codec coverage, the process isolation the runtime lock depends on, and the benchmark harness. **Revisit only if 119.88 fps still misses budget after Tier 2A.** |
| **AV1 export** | Requires 40-series NVENC. HEVC Main10 gets you HDR (**3.13**) and works on everything. Do that first. |

---
---

# What is already strong

Verified during the audit. **Do not "fix" these, and do not re-suggest them.**

### Architecture and protocol

- **The IPC wire format is exemplary.** Magic `0x3152574Eu` + exact-version gate
  (`NeuralWorkerProtocol.h:28`), `static_assert` on all 7 struct sizes (`:167`),
  decoders that reject trailing bytes, odd lengths, oversized counts,
  un-flagged reserved bytes, and overflow. One shared header, not two schemas.
- **No command injection is reachable.** No `cmd.exe` / `system()` / `_popen`
  anywhere. `lpApplicationName` is always a handle-verified absolute path.
  yt-dlp runs with `--no-config --no-cache-dir --no-plugin-dirs --no-playlist`.
  URL is allowlisted (HTTPS + dot-bound host + exact route + 11-char id) **and**
  filtered for quotes, backslashes and control chars.
- **Helper TOCTOU is properly closed** — `FILE_FLAG_OPEN_REPARSE_POINT`,
  canonicalization via `GetFinalPathNameByHandleW` on that same handle, and the
  handle **kept open with `FILE_SHARE_READ` only** across the whole resolve.
- **No cache path ever comes off disk.** The manifest has no path fields; every
  path is `root/<bucket>/<hex-digest>` re-checked with `weakly_canonical` + a
  component-wise descendant test.
- **Publication is a directory rename** with full SHA-256 of payload and both
  sidecars on every read, plus a double manifest re-parse-and-compare.
- **`SegmentWriter`'s three-thread handshake is sound** — every interleaving of
  Cancel/Finish/Fail/Rotate/ClaimWarm/Finalize was walked; no lost wakeup, no
  deadlock, no orphaned armed encoder.
- **`CompletionRegistry`** is the right cross-thread pattern — posted messages
  carry scalar tokens, never addresses.
- **`ParallelFor.h`'s pool is correct** — the `busy_` guard, the
  `pending_.wait(remaining)` re-load loop, and `InPoolWorkerFlag` nested-dispatch
  suppression all hold.

### GPU

- D3D12 slot discipline: `WaitForFrameSlot` before every mapped write and
  allocator reset; slot published on every exit after the first
  `ExecuteCommandLists`; `ReleaseDLSSFeatureForIdle` drains before releasing
  (DLSS guide §5.5); `~D3D12Renderer` tears `m_nvof` down while device and queue
  are alive, with the reason in-comment.
- **DRED already wired** (`D3D12Renderer.cpp:200`, `:1427`), queried once on the
  loss transition.
- **GDI is meticulous** — every `CreateSolidBrush`/`CreatePen`/`CreateFontW`/
  `CreateCompatibleDC` site is deselected and deleted, including failure paths.
- NVDEC on **both** playback and offline paths.

### Already-optimized (excluded from Tier 2A)

Capture readback ring (4 persistently-mapped slots, per-slot fence — 0.17.0
measured 48.6 → 109.4 frames/s) · redundant clears removed, depth clear
correctly kept for hi-Z · timestamp readback persistently mapped · guide fan-out
via per-worker semaphores · `CopyMappedRows` single contiguous parallel memcpy ·
decoder pipe sizing and buffer recycling · NV12 over the pipe (14.7 → 5.5 MB per
1440p frame) · `gpuSourceConversion`/`gpuColorConversion` defaults (measured,
priced, decided — leave FALSE) · segment rotation with a warm encoder armed a
segment ahead · offline pipelining sized from measurement · resident-helper
waits event-driven · NVOF on a separate command list with a GPU-side fence.

### Tests and supply chain

- **Test quality is genuinely high, not smoke.** `UpdateCheckTests.cpp:60`
  plants `"tag_name": "...v9.9.9"` **inside a release-notes body** so a naive
  substring parser passes and the correct one is required.
  `PolicyTests.cpp:5459` asserts two seek mechanisms produce **byte-identical
  pixels**. `NeuralPrerenderTests.cpp:759` discovers a genuinely dead PID by
  probing the kernel.
- **Cache corruption is well covered** — payload tamper, in-place manifest
  rewrite, interrupted staging, nonce collision, dead-vs-live owner sweep,
  AV-hold rename retry.
- All 4 GitHub Actions **SHA-pinned**. Every download in `tools/fetch_*.ps1` is
  SHA-256 verified **before** execution **and** re-verified after staging;
  `fetch_youtube_helpers.ps1` additionally runs `--version` on the staged binary.
- Exactly **one** `BCryptOpenAlgorithmProvider` in the whole tree. SHA-256 has a
  single implementation — **this is the pattern the rest of the codebase should
  follow.**
- `tools/verify_package.ps1` enforces an exact file allowlist, per-file
  Authenticode state, and zip-bomb guards (128 entries / 512 MB / ratio 250).
- **CI is real** — builds from a clean runner, runs 13 suites, then assembles
  **and verifies** the package on every PR, so packaging breaks surface on the
  PR not the tag.
- `UpdateCheck` is safe by construction: `WINHTTP_FLAG_SECURE`, 8 s timeouts,
  256 KiB body cap, downgrade protection.

### Hygiene

57/57 headers use `#pragma once`. **Zero `TODO`/`FIXME`/`HACK` in `src/`.** Zero
commented-out code, zero `#if 0`. `WIN32_LEAN_AND_MEAN`/`NOMINMAX` on every
target. No ODR hazards — the three shared-mutable-state sites all correctly use
function-local statics in inline functions. **381 commits in 22 days.**

`docs/TROUBLESHOOTING.md` is outstanding — it quotes literal log lines and tells
the user how to read them.

---

## Method

Six agents, 2026-09-20, against `dec9a87`:

| Agent | Scope |
| --- | --- |
| Competitive research | Topaz, video2x, Anime4K, chaiNNer, SVP, RIFE, FlowFrames, mpv/madVR/VLC, RTX VSR, the DLSS 5 community field. Live GitHub API for all competitor metrics. |
| Player baseline research | HDR/colour, subtitles, audio, decode robustness, presentation timing, crash engineering, distribution trust. Primary sources: Microsoft Learn, mpv/libass/libplacebo/FFmpeg headers, NVIDIA Streamline & NGX, live issue trackers. |
| Correctness audit | Concurrency, D3D12 lifetime, error handling, leaks, untrusted input, integer/buffer, shutdown, cache correctness. |
| Performance audit | Frame path, sync stalls, IPC, pacing, memory, encode path, disk I/O, build flags. |
| Build/test/release audit | Coverage mapping, testability, CI/CD, build system, supply chain, observability, code health. |
| Duplication sweep | Hex formatting, path resolution, SHA-256, JSON, process spawning, string conversion, atomic writes, header hygiene, function length. |

**Note on sourcing:** reddit.com was unreachable in this environment. All
user-demand evidence is from directly fetched Topaz Discourse, the SVP forum,
Doom9, VideoHelp, Plex forums, NVIDIA developer forums and GitHub trackers. No
Reddit content is quoted.
