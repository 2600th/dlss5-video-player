# RTX 5090 verification of the universal runtime `310.8.SF-v2`

Verified 9 September 2026 on a second machine, to confirm commit `a29e9ca`
("run on every RTX generation, and stop wedging after preroll on Ada") on
Blackwell. The commit had been hardware-verified only on an RTX 4080 SUPER
(see [RTX 4080 SUPER record](VERIFICATION-2026-09-09-RTX4080.md)).

Result: the live player session that failed before the fix now completes on
this GPU. One new defect was found, in the keep-up forecast rather than in
neural rendering — see [Higher resolution](#higher-resolution).

## Environment

| Item | Value | Source |
| --- | --- | --- |
| OS | `Microsoft Windows 11 Pro` / `10.0.26200`, 25H2, build `26200.9445` | `Win32_OperatingSystem` + `CurrentVersion` registry |
| GPU | `NVIDIA GeForce RTX 5090`, driver `616.64`, `0x2B8510DE` | `nvidia-smi --query-gpu=name,driver_version,pci.device_id --format=csv` |
| GPU (DXGI) | DXGI driver `32.0.16.1664`, `PCI\VEN_10DE&DEV_2B85&SUBSYS_205710DE&REV_A1` | `Win32_VideoController` |
| Visual Studio | `Visual Studio Community 2022`, `17.14.23` (`17.14.36811.4`) | `vswhere -all -format json` |
| MSVC | toolset `14.44.35207`, compiler `19.44.35222` | `cl.exe` banner; CMake `-- The CXX compiler identification is MSVC 19.44.35222.0` |
| CMake / CTest | `3.31.6-msvc6` | `cmake --version`, `ctest --version` |
| Windows SDK | `10.0.26100.0` | `-- Selecting Windows SDK version 10.0.26100.0 to target Windows 10.0.26200.` |
| Repo commit | `a29e9caf7c7a5b762b05aa0ed35ad3688bec1468` | `git rev-parse HEAD` |
| DLSS SDK | `a291cc7d2cc642a51566f3dfd5376f635cd1b284` | `git -C external/DLSS rev-parse HEAD` |

`vswhere` also reports `Visual Studio Build Tools 2022` `17.14.23` on this
machine. CMake selected the Community instance
(`CMAKE_GENERATOR_INSTANCE:INTERNAL=C:/Program Files/Microsoft Visual Studio/2022/Community`);
`cmake.exe`/`ctest.exe` were run from the Build Tools CMake `bin` directory as
[Building](BUILDING.md) allows, because neither is on `PATH` here.

## Inputs

The pinned SDK checkout was already at the pinned revision, so it was kept as
[Building](BUILDING.md) directs. The three fetch scripts each verified and
staged their pinned downloads:

```
Verified and staged @tabler/icons-webfont 3.46.0 from https://registry.npmjs.org/@tabler/icons-webfont/-/icons-webfont-3.46.0.tgz
Verified and staged yt-dlp 2026.08.19 and Deno 2.9.5.
Verified and staged FFmpeg/FFprobe 9.0.1 in 'D:\Github\dlss5-video-player\external\ffmpeg\bin'.
```

### The locked NR runtime was not the one already on this machine

`external/runtime` on this machine already held eleven of the twelve locked
files at their locked hashes, but its `nvngx_dlssnr.dll` was the **previous
RTX 40-targeted** runtime, not `310.8.SF-v2`:

```
4B8D19BC3EFF58A084F5ECA7489C921501C203450169FB82FF4F649A4482BA05  nvngx_dlssnr.dll                (165840496 bytes)
4B8D19BC3EFF58A084F5ECA7489C921501C203450169FB82FF4F649A4482BA05  nvngx_dlssnr_ada_v51_async.dll  (165840496 bytes)
```

Those two inputs were byte-identical to each other and neither matched the
lock's `6EB209E7…3927` / `165830144`. The pre-existing
`build-upscaling/Release/neural-runtime/nvngx_dlssnr.dll` was the same
`4B8D19BC…BA05` file, so no earlier local run on this machine had exercised
SF-v2.

The locked file was therefore collected from the public source named in the
"Add the experimental runtime" table. The release asset digest reported by
GitHub matches the digest recorded in the lock's `provenance`:

```
browser_download_url: https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.SF-v2/nvngx_dlssnr_310.8.SF-v2.zip
size:   116693212
digest: sha256:1da35941894994eb087e017577829e492454e9bae3a6a9397027069ceb74955c
```

The downloaded archive hashed to that same digest, and its single member
(`nvngx_dlssnr.dll`, `165830144` bytes) extracted to the locked hash:

```
1da35941894994eb087e017577829e492454e9bae3a6a9397027069ceb74955c  nvngx_dlssnr_310.8.SF-v2.zip
6EB209E764F39872625DEBD6ABAF45E2BB6322F6F270F781F70C059AE30B3927  (extracted DLL, 165830144 bytes)
```

It was staged into `external/runtime` under the lock's `sourceName`,
`nvngx_dlssnr_310.8.SF-v2.dll`, which `stage_runtime.ps1` resolves ahead of
the `destination` name, leaving the stale file in place and unused.

### Validator

`tools/stage_runtime.ps1 -InputDirectory external/runtime -Destination external/runtime -ValidateOnly`:

```
Destination               Size SHA256                                                           Authenticode
-----------               ---- ------                                                           ------------
dxgi.dll               5592064 0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7 NotSigned
nvngx_dlss.dll        58956400 C85F971CE023C9F3492FC7455F0B01A24BA18EA39636407A846902C4360B0B7E Valid
nvngx_dlssnr.dll     165830144 6EB209E764F39872625DEBD6ABAF45E2BB6322F6F270F781F70C059AE30B3927 NotSigned
renodx-dlss5.addon64   1732608 D5ADF82EB44B065F4C590AC91FE824BAB07AFEA0EB9F994BDE936710C8593952 NotSigned
sl.common.dll           830592 A4B2B5ACBE49FBC6D44DD432CAC19CD53218F698B2539DC7ED0FB268C72CFC8D Valid
sl.dlss.dll             421504 1EB5FB3D6F01D340FE086D981CC2DE4F18AA6D05EE276E5CF28ECD54818DCC8B Valid
sl.dlss_g.dll           625792 B8B5EFFD7DEBDB750ABD216DE43385FB653261712BC315D85EBA68811FB3EE02 Valid
sl.dlss_nr.dll          401024 9F6672E5E0170DC118A3188D21BDA187E1FC1AA3502895B21AB846D23165C11D Valid
sl.interposer.dll       651392 27B2190057994C0B287C2C5716953BF1586F6499AC12FBBB2092B9AAF8396570 Valid
sl.nis.dll             1155200 6039E38A1AF56C8E86F3E936596E2DB910BF3D76BBF4268562A3B13763049DFA Valid
sl.pcl.dll              360064 12AA4E76C28A27C735E4ECB3072F44D09428ACB107B70AC38E4BD48DDB05F88D Valid
sl.reflex.dll           382080 ECF12973CDCEC2FFCED2EA77B1C7E45F4D387E7C864DDB5531B66A6F947EFFB3 Valid


Verified 12 locked runtime files in 'D:\Github\dlss5-video-player\external\runtime'.
```

All 12 verify, so the neural steps proceeded.

## Build and tests

The pre-existing `build-upscaling` directory was removed first, so that the
warning count and the session logs below describe only this run.

`cmake -S . -B build-upscaling -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON`
then `cmake --build build-upscaling --config Release --parallel`:

- Configure and generate succeeded (`Configuring done (6.1s)`, `Generating done (0.2s)`).
- Build exit 0 in **86.47 s**.
- **Zero compiler warnings**: `grep -ciE 'warning'` over the full 167-line
  build log returns `0`, and there are no `error` lines.

`ctest --test-dir build-upscaling -C Release --output-on-failure`:

```
100% tests passed, 0 tests failed out of 12

Total Test time (real) =  44.84 sec
```

All twelve suites passed: `RecentMediaTests` 0.42 s, `RenderSettingsTests`
0.38 s, `CachedExportTests` 5.65 s, `RuntimeLockTests` 0.22 s,
`RangeSelectionTests` 0.23 s, `FrameIdentityTests` 0.25 s, `NeuralWorkerTests`
0.71 s, `UpscalingTests` 0.21 s, `PolicyTests` 31.09 s,
`ReleaseApiCompileTests` 0.19 s, `NeuralPrerenderTests` 4.30 s,
`PlayerUiRegressionTests` 1.20 s.

Staging into the worker subdirectory reported
`Verified and staged 12 locked runtime files into 'D:\Github\dlss5-video-player\build-upscaling\Release\neural-runtime'.`
with the same twelve hashes as the validator table above, and
`packaging/ReShade.ini` and `packaging/ReShadePreset.ini` were copied beside them.

## GPU smoke

`build-upscaling\Release\MediaGpuSmoke.exe external\ffmpeg\bin build-upscaling\Release\neural-runtime\NeuralWorker.exe smoke-1`
into a non-existent output directory, **exit code 0**.

Every `neural_ok=` line and the verdict from `smoke-1\results.txt`:

```
neural_ok=1 cancelled=0 frames=1 duration_100ns=10000000 native_evaluations=1 verified_frames=1 feature18_armed=1 evidence_valid=1 highest_evaluation=60 elapsed_seconds=8.19798 detail=
neural_ok=1 cancelled=0 frames=100 duration_100ns=10000000 native_evaluations=100 verified_frames=100 feature18_armed=1 evidence_valid=1 highest_evaluation=60 elapsed_seconds=6.74131 detail=
neural_ok=1 cancelled=0 frames=24 duration_100ns=10000000 native_evaluations=24 verified_frames=24 feature18_armed=1 evidence_valid=1 highest_evaluation=60 elapsed_seconds=6.39187 detail=
overall=PASS
```

From `smoke-1\video-ReShade.log`:

```
20:09:15:016 [11372] | WARN  | [DLSS 5 Neural Rendering] DLSS5 Generic: signed runtime sha256 6EB209E764F39872625DEBD6ABAF45E2BB6322F6F270F781F70C059AE30B3927 (custom runtime accepted; untested build, NR failures may be specific to it)
20:09:15:088 [11372] | INFO  | Running on NVIDIA GeForce RTX 5090 Driver 616.64.
20:09:16:315 [11372] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: signed DLSSNR 310.8.0 D3D12 runtime initialized
20:09:16:667 [11372] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: feature 18 created via the signed snippet after DLSS/DLAA for NR input 640x360 -> output 640x360 with guides 640x360
20:09:16:668 [11372] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: inline feature 18 evaluation succeeded (count=1, NR input 640x360 (guides 640x360), output 640x360 [native])
20:09:18:664 [11372] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: feature 18 created via the signed snippet after DLSS/DLAA for NR input 640x360 -> output 640x360 with guides 640x360
20:09:18:669 [11372] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: inline feature 18 evaluation succeeded (count=60, NR input 640x360 (guides 640x360), output 640x360 [native])
```

Every `ERROR`/`WARN` line from `[DLSS 5 Neural Rendering]` in that log — five in
total, two of them `ERROR`:

```
20:09:15:016 [11372] | WARN  | [DLSS 5 Neural Rendering] DLSS5 Generic: signed runtime sha256 6EB209E764F39872625DEBD6ABAF45E2BB6322F6F270F781F70C059AE30B3927 (custom runtime accepted; untested build, NR failures may be specific to it)
20:09:16:143 [11372] | ERROR | [DLSS 5 Neural Rendering] vtable::Hook(Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C)
20:09:16:143 [11372] | ERROR | [DLSS 5 Neural Rendering] vtable::Hook(Failed to find NVSDK_NGX_D3D12_EvaluateFeature_C)
20:09:19:493 [11372] | WARN  | [DLSS 5 Neural Rendering] vtable::Unhook(NVSDK_NGX_D3D12_EvaluateFeature_C not hooked, skipping)
20:09:19:493 [11372] | WARN  | [DLSS 5 Neural Rendering] vtable::Unhook(NVSDK_NGX_D3D12_EvaluateFeature_C not hooked, skipping)
```

The two `ERROR` lines are the only `ERROR` lines anywhere in the log. They did
not affect the result: `EvaluateFeature_C` is never hooked, the inline path is
used instead, and it is the inline path that reports
`inline feature 18 evaluation succeeded (count=60…)`. The matching `Unhook`
`WARN`s at teardown are the same fact. The same five lines, with the same
meaning, appear in the live-session log below.

`smoke-1\video-DLSSVideoPlayer.log` carries **seven** `Render stage` lines, not
six — `unaccounted` is reported alongside the six named stages:

```
[20:09:19.159] Render stage read n=24 mean=2.25514 p50=0.0033 p95=0.007 totalMs=54.1233
[20:09:19.159] Render stage guide n=24 mean=0.900546 p50=0.8425 p95=1.3306 totalMs=21.6131
[20:09:19.159] Render stage render+capture n=24 mean=3.23155 p50=3.0815 p95=3.8768 totalMs=77.5572
[20:09:19.159] Render stage submit(guide+render+gate) n=24 mean=81.9172 p50=4.1763 p95=5.0778 totalMs=1966.01
[20:09:19.159] Render stage write n=24 mean=6.85268 p50=0.0849 p95=0.117 totalMs=164.464
[20:09:19.159] Render stage frame n=24 mean=91.036 p50=4.3133 p95=166.872 totalMs=2184.86
[20:09:19.159] Render stage unaccounted n=24 mean=0.0109708 p50=0.0086 p95=0.0169 totalMs=0.2633
```

The render+capture mean of 3.23 ms at 640x360 is close to the 3.97 ms the RTX
4080 SUPER record quotes for the same stage and clip.

## Live session

`build-upscaling\Release\DLSSVideoPlayer.exe "…\docs\media\neural-comparison-demo.mp4"`
(1920x1080, `30/1` fps, 900 frames, 30.000000 s per `ffprobe`). `D` was pressed
once 3.016 s after the window appeared, held 61.093 s, pressed again, and the
player was closed 5.20 s later; it exited cleanly with code 0. Keystrokes were
posted to the player's window by a small compiled driver, because
`Add-Type`-based PowerShell input injection is blocked by this machine's
antivirus.

This is the case that failed twice in two attempts on the RTX 4080 SUPER before
the fix. **It succeeded here.**

```
[20:12:33.349] D3D12 adapter vendor=0x10de device=0x2b85
[20:12:35.531] Active neural session started at 1.03333 s through 30 s.
```

All fifteen `Neural segment` lines:

```
[20:12:45.381] Neural segment 0 frames=60 firstFrame=31 span=[1.03333,3.03333) file=neural-00000.mkv
[20:12:46.005] Neural segment 1 frames=60 firstFrame=91 span=[3.03333,5.03333) file=neural-00001.mkv
[20:12:46.693] Neural segment 2 frames=60 firstFrame=151 span=[5.03333,7.03333) file=neural-00002.mkv
[20:12:47.503] Neural segment 3 frames=60 firstFrame=211 span=[7.03333,9.03333) file=neural-00003.mkv
[20:12:48.251] Neural segment 4 frames=60 firstFrame=271 span=[9.03333,11.0333) file=neural-00004.mkv
[20:12:49.001] Neural segment 5 frames=60 firstFrame=331 span=[11.0333,13.0333) file=neural-00005.mkv
[20:12:49.725] Neural segment 6 frames=60 firstFrame=391 span=[13.0333,15.0333) file=neural-00006.mkv
[20:12:50.442] Neural segment 7 frames=60 firstFrame=451 span=[15.0333,17.0333) file=neural-00007.mkv
[20:12:51.163] Neural segment 8 frames=60 firstFrame=511 span=[17.0333,19.0333) file=neural-00008.mkv
[20:12:51.816] Neural segment 9 frames=60 firstFrame=571 span=[19.0333,21.0333) file=neural-00009.mkv
[20:12:52.565] Neural segment 10 frames=60 firstFrame=631 span=[21.0333,23.0333) file=neural-00010.mkv
[20:12:53.221] Neural segment 11 frames=60 firstFrame=691 span=[23.0333,25.0333) file=neural-00011.mkv
[20:12:53.934] Neural segment 12 frames=60 firstFrame=751 span=[25.0333,27.0333) file=neural-00012.mkv
[20:12:54.654] Neural segment 13 frames=60 firstFrame=811 span=[27.0333,29.0333) file=neural-00013.mkv
[20:12:54.998] Neural segment 14 frames=29 firstFrame=871 span=[29.0333,30) file=neural-00014.mkv
```

Count: **15 segments**, fourteen of 60 frames and a 29-frame tail = 869 frames.

Computed pace, from the thirteen intervals between the fourteen consecutive
60-frame segments (segment 0 at `20:12:45.381` to segment 13 at `20:12:54.654`
= 9.273 s):

- mean interval **0.7133 s** per 60 frames
- **11.8885 ms/frame**

That is independent of, and agrees with, the player's own figure of
`11.888039` ms/frame (below) to 0.0006 ms.

```
[20:12:56.463] Neural render receipt: gpu="NVIDIA GeForce RTX 5090" driver=32.0.16.1664 reshade=6.8.0.2155 renodx=4.7 nr=310.8.0 feature18=armed lock=ok failure=none frames=869/869 verified=869 resets=3 retries=0
[20:13:01.561] Active neural session rendered 869 frames and published its cache entry; save=1 entry=D:\Github\dlss5-video-player\build-upscaling\Release\cache\v1\renders\8f0cd3f7b8d343ff4494acd2e521a4cb18541e18f086250bc80855c301b42598\neural.mkv
[20:13:36.645] Measured neural render pace: 1920x1080 at 11.888 ms/frame over 809 frames (0.948463x the reference GPU).
[20:13:36.655] Active neural session stopped at 29.9667 s; presented=864 dropped=5
```

`presented=864 dropped=5`. Playback attached and stayed ahead:

```
[20:12:46.859] Active neural playback attached at 1.03333 s with 5.99999 s buffered.
[20:12:46.862] Neural buffer filled at 1.03333 s with 5.99689 s ahead; resume=1
```

The fifteen segments spanned 9.617 s of wall time and the receipt landed at
`20:12:56.463`, about 21 s after the `20:12:35.397` toggle, so the session
finished well before the 60 s hold elapsed. The second `D` press stopped an
already-completed session, which is what wrote the pace line and the INI.

`[NeuralPace]` from `build-upscaling\Release\DLSSVideoPlayer.ini`:

```
[NeuralPace]
Gpu=NVIDIA GeForce RTX 5090
MsPerFrame=11.888039
Width=1920.000000
Height=1080.000000
```

`build-upscaling\Release\neural-runtime\DLSSVideoPlayer.log` contains **no**
`GPU fence wait failed` line (`grep -c` returns `0`), and no `error`, `fail` or
`abort` line at all. The session did not fail, so no `ReShade.log` tail is
quoted; the worker's `ReShade.log` confirms SF-v2 was the runtime actually
loaded by the player's worker:

```
20:12:41:139 [31548] | WARN  | [DLSS 5 Neural Rendering] DLSS5 Generic: signed runtime sha256 6EB209E764F39872625DEBD6ABAF45E2BB6322F6F270F781F70C059AE30B3927 (custom runtime accepted; untested build, NR failures may be specific to it)
20:12:41:174 [31548] | INFO  | Running on NVIDIA GeForce RTX 5090 Driver 616.64.
20:12:42:418 [31548] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: signed DLSSNR 310.8.0 D3D12 runtime initialized
20:12:42:755 [31548] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: feature 18 created via the signed snippet after DLSS/DLAA for NR input 1920x1080 -> output 1920x1080 with guides 1920x1080
20:12:44:413 [31548] | INFO  | [DLSS 5 Neural Rendering] DLSS5 Generic: inline feature 18 evaluation succeeded (count=60, NR input 1920x1080 (guides 1920x1080), output 1920x1080 [native])
```

### Comparison

| Reference | ms/frame | This machine / reference |
| --- | --- | --- |
| RTX 5090 model reference, 1080p | 12.50 | **0.951x** |
| RTX 4080 SUPER measurement | 15.31 | **0.777x** |

Both ratios use the computed 11.8885 ms/frame. The 4080 figure is quoted as
`15.31` above and as `15.3055` in that record; the ratio is `0.7765` against
the former and `0.7768` against the latter, so 0.777x either way.
The player's own scale against the fitted model, `0.948463x`, is a different
quantity: it divides by the model's 1080p *prediction* of 12.534 ms
(`7.35 + 2.0736 × 2.50`), not by the 12.50 ms datum.

So at 1080p this RTX 5090 lands within 5% of the model's reference point, and
renders about 1.29x as fast as the RTX 4080 SUPER.

## Higher resolution

No 4K30 or 1440p file existed on this machine — every local candidate probed
was 1080p or smaller (1280x720, 1024x576, 1920x1032, 1080x1920, 1920x1080).
Both higher-resolution clips were therefore **synthesized** from the same
demo source with the staged FFmpeg 9.0.1
(`scale=…:flags=lanczos -r 30 -c:v libx264 -preset veryfast -crf 18`),
900 frames and 30.000000 s each, confirmed by `ffprobe` as `2560,1440,30/1,900`
and `3840,2160,30/1,900`. Each was run exactly as the 1080p session was.

| Clip | Segments | Frames | Player pace (ms/frame) | Scale vs model | presented / dropped | Keep-up prompt |
| --- | --- | --- | --- | --- | --- | --- |
| 1920x1080 30 | 15 | `869/869` verified | `11.888039` | `0.948463x` | `864` / `5` | not shown |
| 2560x1440 30 | 15 | `868/868` verified | `17.149086` | `1.0352x` | `855` / `12` | not shown |
| 3840x2160 30 | 15 | `869/869` verified | `42.869881` | `1.52638x` | `21` / `848` | **not shown** |

```
[20:19:37.715] Measured neural render pace: 2560x1440 at 17.1491 ms/frame over 808 frames (1.0352x the reference GPU).
[20:19:37.721] Active neural session stopped at 29.9667 s; presented=855 dropped=12
[20:17:52.194] Measured neural render pace: 3840x2160 at 42.8699 ms/frame over 809 frames (1.52638x the reference GPU).
[20:17:52.201] Active neural session stopped at 29.9667 s; presented=21 dropped=848
```

Both sessions rendered every frame and verified every frame
(`feature18=armed lock=ok failure=none frames=868/868 verified=868 resets=3 retries=0`
and `frames=869/869 verified=869 resets=3 retries=0`), and neither worker log
contains `GPU fence wait failed`. Neural rendering itself is sound at all three
resolutions.

### Defect: the keep-up forecast does not warn at 4K30

The player **did not** show the "cannot keep up" prompt before starting either
higher-resolution session, and quoted no rate, because
`ForecastLiveRender` predicted that both would keep up. At 4K30 that prediction
was wrong and the user got no warning: the session then dropped
**848 of 869** frames (`presented=21 dropped=848`).

The cause is the single-scalar pace model in `src/PlaybackTiming.h`. The stored
scale came from the 1080p run (`0.948463x`), so the 4K forecast was
`0.948463 × 28.086 = 26.6 ms/frame`, i.e. 37.5 fps, 1.25x realtime — above the
`0.98` threshold, so `keepsUp` was true and no `MessageBoxW` was raised. The
measured cost was `42.8699` ms/frame, i.e. 23.3 fps, **0.778x realtime**.

The scalar is not constant across resolution on this GPU. Measured scales are
`0.948463x` at 1080p, `1.0352x` at 1440p and `1.52638x` at 4K — the very
assumption the comment at `PlaybackTiming.h:48-51` relies on ("One scalar on
the reference cost is enough"). A least-squares fit of this machine's three
points is `0.015 ms + 5.113 ms/MP` (residuals `+1.270`, `-1.715`, `+0.445` ms),
against the model's `7.35 ms + 2.50 ms/MP`: the fixed term is essentially zero
here and the per-megapixel term roughly doubles.

The measured 4K cost is also 52.7% above the `28.07` ms/frame the model's
comment attributes to an RTX 5090 on driver 616.64, while 1080p and 1440p sit
within 5% (`11.888` vs `12.50`; `17.149` vs `16.60`). [INFERENCE] Part of that
gap is likely the synthesized source rather than the GPU: the 4K re-encode is
6315 kbit/s against the 1080p demo's much lower rate, and the segment-arrival
metric includes decode and encode, not just render. This machine's 4K number
should not be treated as an apples-to-apples replacement for the model's 4K
datum.

The practical consequence stands regardless of that caveat: on this GPU, 4K30
does not keep up, and the current forecast starts the session anyway.

## Verdict

**Neural rendering works on the RTX 5090 with `310.8.SF-v2`.**

Steps 2 through 5 all passed: all 12 locked files verified, a clean warning-free
Release build, 12/12 CTest suites, `overall=PASS` from the strict media smoke
with SF-v2's hash accepted and inline feature 18 evaluating 60 frames, and the
live player session that previously failed on Ada completing with
`frames=869/869 verified=869`, `failure=none`, `presented=864 dropped=5` and no
`GPU fence wait failed`. Blackwell is confirmed on the universal build, closing
the first item in the RTX 4080 SUPER record's Limits section.

The one defect found is in the keep-up forecast, not in neural rendering: see
[Higher resolution](#higher-resolution).

## Limits

- The locked SF-v2 runtime was not present on this machine and had to be
  fetched; the eleven other locked files were already correct. That the
  download reproduced the locked hash confirms the lock, but it means this run
  did not re-derive `dxgi.dll` from `ReShade_Setup_6.8.0_Addon.exe` — the
  existing `0CEE63F9…94F7` file was accepted by the validator as-is.
- The 1440p and 4K clips are re-encodes of the 1080p demo, not native captures.
  Their absolute ms/frame includes the higher decode cost of those re-encodes.
  No native 4K or 1440p source was available here.
- One 30 s clip per resolution, one session each; no repeat runs, so run-to-run
  variance is unmeasured. The 1080p session was run once and passed once,
  against the 4080's two-of-two pre-fix failures.
- The 4K30 keep-up gap was observed during this run and not fixed in it. It
  was fixed afterwards in `e16b1c4` (per-geometry pace samples in
  `src/PlaybackTiming.h`), and
  `live_render_forecast_predicts_from_this_gpu_measured_geometries_test` in
  `tests/NeuralPrerenderTests.cpp` replays this machine's three measurements.
  The 4K session itself was not re-run after the fix.
- Only the neural path was exercised at these resolutions. Super Resolution,
  the range/offline render path, YouTube input, packaging
  (`package_release.ps1` / `verify_package.ps1`) and visual quality were not
  checked; CTest does not establish visual quality.
- Turing (RTX 20) and Ampere (RTX 30) remain unverified — no such hardware
  here — as does Ada on this particular machine.
- The keystrokes were posted programmatically to the player window rather than
  typed on a physical keyboard, and the player ran windowed at its default
  size; fullscreen and real input hardware were not exercised.
