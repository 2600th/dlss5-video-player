# Changelog

## 0.21.2 - 2026-09-12

- A live session whose first finalized segment starts after the playhead now
  attaches instead of restarting forever. Toggling neural rendering on at
  12.0329 s published coverage from 12.0662 s - one 30 fps frame later - and the
  attach demanded a segment containing the playhead, so it failed, the
  stalled-attach recovery restarted the session at the same instant, and that
  repeated: `never attached at 12.0329 s with head 14.5661 s over 2 segments
  after 20 attempts` five times in 25 seconds, with the picture stuck on one
  frame while the render kept publishing. Playback now joins at the first
  rendered frame, which is what continuing from there means, and the recovery is
  bounded: one restart at the playhead, then the session ends and hands back the
  original instead of looping. `live_session::AttachPosition100ns` decides it, so
  it is tested without a window.
- **Video ▸ Compare ▸ Wipe** survives bright content. The divider was a single
  white pixel column, so it vanished into a white shirt or a sky - the one thing
  the mode exists to show. It is now a white core inside a dark edge, so one of
  the two always has contrast. Presentation only: exports and cached frames are
  unchanged.
- A comparison mode refused for want of a resident pair says so in the log
  (`Comparison mode refused: loaded=1 cachedPair=0 neuralView=1`), and an
  accepted one records the mode, the divider position, the zoom and whether the
  reference is uploaded. A menu command that silently did nothing was
  indistinguishable from one that did something invisible.
- A play press made while the buffer fills is visible on the control: the button
  reads `Pause` with the press remembered, and the status line says whether the
  fill will start playback or stay paused. `m_playing` is false during a fill, so
  the button kept reading `Play`, which read as "the press did nothing" and the
  obvious second press cancelled the first.
- A neural toggle pressed during a seek is queued and applied when the seek
  lands, instead of being dropped with only a log line. The toolbar says
  `Neural Rendering · Queued for the seek` while it waits.

## 0.21.1 - 2026-09-12

- A finished render is no longer thrown away because something else had the file
  open for a second. Publishing an entry is a directory rename, and Windows
  refuses to rename a directory while any file inside it is open - which is
  exactly what an antivirus scanner does to a 186 MB file the moment it is
  closed. One attempt was made, so a 2871/2871-verified 1440p render ended on
  `The neural video failed final cache validation` while every number the gate
  printed agreed: `probeFrames=2870 resultFrames=2870`, all three durations
  within 17 ms of a 102 ms tolerance. The staging directory could not even be set
  aside afterwards, which is what named the cause: both renames of the same
  directory failed at the same instant, and it renamed cleanly by hand once the
  process exited. Transient sharing errors are now retried for up to 3 s
  (24 attempts, 125 ms apart), and the entry publishes.
- The publish refusal says which half refused. `gate=` covers the evidence and
  duration checks; `promoteStage=` names the step inside the promotion -
  `payload-digest`, `manifest-reread`, `rename`, `reopen` - with the Win32 error
  and the number of rename attempts. A failed set-aside now logs too, instead of
  leaving a staging directory with no explanation. Proven end to end: with a
  handle deliberately held on the finished entry, the log reads `Neural cache
  entry published after 20 rename attempts` and the session publishes 2875/2875
  frames; the same condition destroyed the render before this change.
- New README video and screenshots, from Grand Theft Auto VI and The Godfather.
  The 30-second demo is now two live 2560x1440 sessions recorded as they ran: the
  real `Ctrl+Alt+D` toggle on a paused Godfather frame, then 15 seconds of GTA VI
  playback with the neural view left on. The paused original/neural pairs, the
  two new unscaled face figures and a shot of the new **Neural strength** dial
  come from the same sessions. Rockstar's own *An Extended Look* upload is
  age-restricted and no anonymous client can fetch it at any resolution, so the
  GTA VI footage is Netflix's *Now Playing* cut of the same material. Captures
  now use the window's visible frame (`DWMWA_EXTENDED_FRAME_BOUNDS`) instead of
  `GetWindowRect`, which had been dragging a strip of the desktop into the shot.
  Provenance, digests, frame numbers and crops: `docs/screenshots/README.md`.

## 0.21.0 - 2026-09-12

- A completed neural render no longer loses its cache entry at the publish gate.
  A live session's entry is `ConcatenateMedia`'s join of the segment files the
  render published, and the join took **every** segment in the index while the
  manifest and the gate described only the last job. A session that rebased or
  re-attached therefore labelled a file it did not match: the diagnostic added
  below caught one joining 46 files of 2622 frames and 87.4 s against a result of
  1647 frames and 54.9 s, and the gate correctly refused a render that had just
  verified every frame it produced. The join now takes exactly this job's
  segments, starting at the index the session handed it, so the file matches its
  own label; earlier coverage stays in the index for playback to keep reading.
  The refusal was also undiagnosable, so it now logs every number it judged
  (both geometries, both frame counts, all three durations, the tolerance and the
  part count) before it gives up. Live proof on an RTX 5090 / 616.64:
  `frames=2656/2656 verified=2656` and `frames=2082/2082 verified=2082` sessions
  both `published its cache entry`
  (docs/VERIFICATION-2026-09-12-RTX5090.md).
- The publish tolerance is also sized to the join now
  (`RuntimePolicy::JoinedMediaDurationTolerance100ns`): one frame of playback
  jitter plus one 1 ms Matroska rounding per joined file, 343334 ticks for a
  single file and 773334 for a 44-part join at 30 fps, because those roundings
  sum across a concatenation instead of cancelling. The frame count is still
  compared exactly, since a join that loses a frame is a real defect.
- Re-toggling neural rendering at full coverage no longer starts a doomed job.
  The session head is an integer number of frames and the range end is a probed
  duration, so a residual below one frame passed the "is there anything left to
  render" guard and spawned a worker that immediately refused its own range with
  `The requested render range lies outside the source`.
  `RuntimePolicy::RenderRangeIsCovered` now treats a remainder shorter than one
  frame as coverage. It was reachable through the publish failure above, which is
  how it was found.
- The render now refuses a run that produced frames without running the neural
  pass. The evidence chain checked that feature 18 was created, evaluated and
  that every frame was verified, all of which a DLAA-only run satisfies: one
  reported `frames=900/900 verified=900` at 0.46 ms of neural GPU time per frame
  against a healthy 5.7 ms. The median it cannot fake was already measured and
  already in `receipt.json`, so `NeuralTimingClearsFloor` now requires
  0.59 ms per output megapixel, 1.223 ms at 1080p. That is the geometric midpoint
  of the 0.46 ms failure and the 3.26 ms lowest healthy median on record, 2.66x
  from each, and the risk is one-sided: per-pixel cost only rises on slower
  hardware. A build with no timing instrumentation reports zero samples and is
  still accepted.
- New **Neural strength** dial in the image adjustments window, 0 to 200 percent,
  which re-composes the frame already on screen instead of re-rendering it. Every
  existing strength-like control is a model parameter in `ReShade.ini`, and that
  block is hashed into the render identity, so changing one costs a full render:
  a median of 10.6 s per cold single-frame preview against 1.03 s from cache.
  The add-on overwrites the neural output in place, so there is nothing to
  re-evaluate, but the presentation shader already holds the composed neural
  frame and the original side by side for the comparison modes. Below 100 percent
  the dial mixes back toward the original; above it, it extends the luminance
  ratio the model produced - a ratio, never an additive delta, with a two-sided
  guard, a 1/512 floor, one scalar across the triple and peak normalisation on
  encode (RenoDX's rules, MIT, read directly). At 100 percent the composite is
  not entered, and the capture and export paths force it there, so no cached
  render, export, digest or cache key changes. Measured on one paused frame:
  52.9 % of pixels move at 0 percent, 51.7 % at 200 percent, and of the pixels
  the model itself moved, 99.8 % follow its direction under the extension.
- Roadmap item 0 is closed. The report that `renodx-dlss5` 4.6/4.7 faults on
  every evaluate from driver 616.64 did not reproduce in six live sessions on an
  RTX 5090 on that driver with the pinned stack: `2805/2805`, `2779/2779`,
  `2697/2697`, `2607/2607` verified, `failure=none`, `lock=ok`, ~7.0 ms/frame.
  The pin stands and the warning against blind runtime upgrades stands;
  `docs/ECOSYSTEM_REVIEW.md` also had its citations re-verified against 0.20.1,
  29 of which had moved since v0.17.1.
- YouTube **Auto** no longer pins the lowest-bitrate rung YouTube offers. It
  asked for exactly 1080p, which on YouTube is the bottom of the ladder:
  measured with the bundled yt-dlp on one trailer, 1080p is 3899 kbps, 1440p is
  7854 and 2160p is 20764, and on a second trailer 4604 / 9282 / 18971. Auto now
  takes the tallest rung up to 1440p and the highest advertised bitrate inside
  it, so the same trailer arrives at about twice the bitrate for about 1.8x the
  render cost. The cap is 1440 and not 2160 on purpose: 4K is 42 ms/frame, 0.78x
  real time, and four times the VRAM and cache footprint. 2160p remains an
  explicit choice in **Video > YouTube source quality**.
- The player now says what the stream actually is, instead of playing a
  degraded one silently. A session reported here played an age-restricted
  trailer at `640x360` and `451 kbps` and rendered at that resolution, with
  nothing on screen to explain it: YouTube exposes one legacy progressive format
  to an anonymous session for those videos, and which one you get is not stable
  between calls. The resolver now reports the selected height, its bitrate and
  the age gate; the log names them, and the status line names them too when the
  height is below 720, with the sign-in reason when the video is age-restricted.
  Three of the six bundled example trailers are age-restricted
  (docs/EXAMPLE_VIDEOS.md).
- A recent-history entry that names a source copy the cache no longer holds is
  ignored instead of failing a render. Clearing the cache folder, or an
  acquisition that never finished, left the key behind; a job handed that key
  reported a missing source, and a live session ended on it with no message at
  all. The key is now verified against the cache before it is used, a job that
  still finds the copy gone re-acquires the stream it is already playing rather
  than giving up, and the case that genuinely cannot recover carries a sentence.

## 0.20.1 - 2026-09-12

- Live neural playback no longer drops half its frames. A session plays the
  render as two-second segment files, and the next file was opened on the thread
  that presents frames - `VideoDecoder::Open` starts `ffprobe` as a child
  process and waits for it. ffprobe.exe is 98 MB, so on a machine whose
  antivirus scans a process start that probe costs 684 ms where the same probe
  inside a scanner exclusion costs 32 ms, and the player paid it every two
  seconds. On an RTX 5090 / 616.64, a 94.7 s 1080p30 YouTube trailer played
  `presented=1415 dropped=1110`: 47 boundary opens at a median of 732 ms spent
  36.5 s of 84 s of playback inside a segment open, and every frame that came
  due in there was more than 1.5 frame intervals late and dropped, which also
  reset the guides and the neural history. Two changes: segments after the first
  are opened with the parameters the first one probed - same encoder, same
  geometry, same frame rate - through the new `VideoDecoder::OpenKnown`, and the
  open moved to a worker thread the boundary only waits for when the one-second
  prefetch lead was not enough. Same clip, same folder, same antivirus:
  `presented=2838 dropped=1`, 4 probes instead of 47, boundary opens at a median
  of 9 ms. The render was never the problem - the same sessions verified
  2805/2805 and 2779/2779 frames at ~7.0 ms/frame
  (docs/VERIFICATION-2026-09-12-RTX5090.md).

- Turning neural rendering on stopped taking sixteen seconds. Measured on an
  RTX 5090 / 616.64 from the key press to the first neural frame on screen:
  **14.71 s -> 9.24 s** in a packaged install whose antivirus scans every
  process start, and **11.43 s -> 5.80 s** in a scanner-excluded one. Four
  things were paying for answers already in hand:
  - The feature-18 preflight - a whole second helper process that loads
    ReShade, the add-on and NGX, creates feature 18 and exits, 4.4 s of it -
    ran on every toggle. `NeuralPreflightLatch` remembered failures only; a
    pass was recorded and never read. It now remembers the pass and its
    receipt, and keeps it beside the render cache keyed on GPU, driver and the
    runtime digest, so a fresh launch on the same machine skips the probe. The
    reused receipt is stamped `reusedVerdict` rather than passed off as a probe
    this session ran, and a render whose runtime changed behind that key still
    fails on its own armed-evidence check.
  - The 226 MB locked runtime was SHA-256'd three times per session
    (`BuildRuntimeDigest`, `VerifyRuntimeLock`, `DescribeRuntimeModules`).
    `Sha256FileCached` memoises installation files on (path, size, write time);
    user content still goes through the uncached `Sha256File`.
  - Nothing could be shown until the first two-second segment was muxed, and
    then not until four seconds of lead existed. The first segment is now half
    a second (`--first-segment-frames`; later ones stay at two), and the lead
    scales with the measured pace - a GPU that renders 4.8x faster than real
    time refills the buffer faster than playback drains it, so it attaches on
    one second instead of four.
  - Two probes were run on files that had already been probed: the job's
    metadata read started a full ffmpeg child it closed two lines later
    (`VideoDecoder::OpenMetadata` now runs the probe alone), and the live
    attach re-probed the original the player already had open (it takes the
    player's `KnownMedia`). On the scanned install those two were ~1.7 s.

- `MediaGpuSmoke` measures a complete decoded frame against the layout the
  decoder opened, not against four bytes per pixel. 0.19.0 moved the export's
  sequential decode to NV12 and the gate has failed on every run since - ten
  `Export contains an incomplete decoded frame` lines and `overall=FAIL` on an
  otherwise healthy render. It reports `overall=PASS` again.

- The Optical Flow SDK licence notice now ships in both packages.
  `THIRD_PARTY_LICENSES/nvidia-optical-flow-MIT.txt` covers the MIT grant on the
  two interface headers vendored in `external/nvof`, and `THIRD_PARTY.md` has
  pointed at it since 0.20.0, but it was in neither package allowlist - so the
  published 0.20.0 core zip references a notice it does not contain. The file
  itself is in the repository and in the complete package; only the core zip is
  affected, and it is left as published rather than replaced under a tag that is
  already out.

## 0.20.0 - 2026-09-11

- Motion vectors now come from NVOFA, the optical flow engine that has been
  sitting idle on every RTX card since Turing. The CPU estimator analysed a
  160x90 grid, so one vector covered 24x24 source pixels at 1080p and its finest
  step was three of them; measured against a synthetic pan on non-periodic
  content it accepted 0 % of its cells at 0.5 px/frame and 2 % at 1.0, and a
  vector it declines to emit is not "unknown" to the reconstruction, it is a
  claim that nothing moved. The engine works on a 2x2 grid in S10.5 fixed point
  - 960x540 vectors, a thirty-second of a pixel - and dlss5-bridge measured it
  answering 0.156 at a true 0.10 px where a shader-based estimator reports zero.
  Uploads and the flow capture moved into their own command list so the queue
  can be signalled past them and the engine started without stalling the CPU;
  the frame's list waits on the engine's fence on the GPU. Costs 1.7 ms/frame at
  1080p (8.92 -> 10.62 ms loop). `TemporalGuideGenerator` keeps the depth proxy
  and the cut detector, and keeps motion too on a card or a build without the
  engine.
- Sampling jitter is gone. Every frame was resampled by a Halton offset and that
  offset reported to NGX, which is what a game does - but a game jitters its
  projection and genuinely samples new points of a continuous scene, while a
  decoded frame is a fixed grid of samples and shifting it bilinearly only
  convolves it with a tent whose width changes every frame. The Nyquist gain of
  that tent swings from 1.0 to 0.0625 across sixteen frames. Rendering 120
  frames of a still image through the real neural path before and after: mean
  frame-to-frame luma difference 0.158 -> 0.099, per-pixel temporal standard
  deviation 2.23 -> 1.41, and at the 99th percentile - where visible shimmer
  lives - 11.78 -> 4.61. Feature 18 has no jitter input at all, so nothing
  downstream was undoing it either.
- Tearing is no longer requested on a window anyone looks at. Both present paths
  asked for `DXGI_PRESENT_ALLOW_TEARING` unconditionally; that belongs to the
  hidden swapchain the offline carrier presents into purely so the add-on sees a
  present per frame, where holding to the display refresh would cap an export
  that already runs below real time. Playback keeps `syncInterval 0`, so its
  pacing is unchanged - it just scans out whole frames now.
- Opening a YouTube video while a live neural session was running left the
  session, its segments and the synchronized pair attached to the new source.
  `Tick` reads frames from that pair for as long as `m_cachedPlayback` is set,
  so the newly swapped decoder was never read: audio ran, the picture sat on the
  first frame, and the seek bar kept painting the previous video's rendered
  span. `InstallPreparedYouTube` was the only source swap that never reached the
  teardown `Unload` performs.
- Toggling neural rendering off and on re-rendered everything instead of
  resuming. Retained coverage is keyed on source, settings and guides, and for a
  YouTube source that key used the signed media URL - which is reissued by every
  resolution, including the one the toggle's own seek performs on the video
  already playing. It now keys on the page URL, which is what Recent and the
  source cache already use. A 100-second head survives a toggle.
- A refused neural render says why. The reason was written to the log and then
  dropped, so on a driver below the 610.47 floor the toggle appeared to do
  nothing: the original kept playing and the only explanation was in a file. The
  one-line reason now leads the status bar, and the driver notice - detected,
  minimum and verified versions - is shown once per session, since the refusal
  repeats on every play, seek and settings change and a dialog on each of those
  would be worse than silence.

## 0.19.0 - 2026-09-10

- The neural export moved its transport onto the GPU's own engines, from
  ctype-lab's `optimize-neural-pipeline` work (PR #6, squashed). NVDEC decodes
  the source, the pipe carries NV12 instead of BGRA (4.2 MB rather than 11.1 MB
  per 2578x1080 frame, `src/PixelLayout.h`), NVENC keeps its CUDA context from
  the spawn rather than the first encoded frame, and the local-file queue thread
  blocks in `ReadFile` instead of poll-sleeping. Measured by the author on an
  RTX 5070 Ti / 616.64 at 2578x1080: 7.82 -> 4.16 ms/frame with presents off,
  per-segment first-frame stall 126 -> 59 ms, 389 encoder write stalls of >=10 ms
  -> 0. With presents restored the loop is the NGX evaluate itself at ~8.35 ms,
  so the remaining pipeline overhead is ~0.4 ms/frame.
- New DLSS > Encoder settings window (`[Encoding]` in the ini): NVENC preset
  p1..p7, GPU capture conversion, GPU source conversion. It is its own window
  because these apply to the next render, unlike the model settings whose Apply
  restarts the session. The three settings dialogs are resizable.
- GPU source conversion ships off, against the contributed default. The NV12
  source shader applies a fixed BT.709 limited-range inverse while nothing
  probes the source's matrix or range (ffprobe is not asked for `color_space`
  or `color_range`), so a BT.601 or full-range clip would reach the model with
  shifted colour - and the flag is deliberately not in the cache key, which
  would make that unrecoverable without a manual cache purge. It stays opt-in
  until the probe exists. `--gpu-source-conversion` was also inverted on the
  wire for the same reason: absent now means the CPU conversion every earlier
  helper performed, so an old parent driving a new helper cannot be switched
  onto the new decode path behind its back.
- `hevc_nvenc` asks for `-split_encode_mode auto`, not `forced`. FFmpeg hands
  the value straight to `nvEncInitializeEncoder` with no capability gate, and
  this player only sees a rejected encoder open as a broken pipe on the first
  frame - which retries the whole render on libx264, silently turning an HEVC
  export into H.264. Only a dual-NVENC Blackwell has been measured; `auto` lets
  the encoder stripe where striping exists.
- Fixed a use-after-free reachable on any GPU: tooltip text is handed to
  `TTM_ADDTOOL` as a pointer, and the store was shared by all three settings
  dialogs, so opening a second dialog freed the first one's strings while its
  tooltips were still subclassed onto its controls. Hosts and text are now kept
  per dialog.
- Settings dialogs size themselves through `AdjustWindowRectExForDpi`. The
  process is per-monitor-aware, so the 96-dpi frame metrics left the client area
  short on a scaled monitor and the bottom-anchored buttons overlapped the note
  text; on this 168-dpi machine the encoder dialog now reports its exact 466x232
  design client, which `PlayerUiRegressionTests` asserts.
- A cancelled decoder read keeps the bytes it already copied out of the pipe.
  Frames there are delimited by byte count alone and `SeekSeconds` reuses the
  running child, so discarding them would have shifted every later frame for
  the rest of the session.
- `PixelLayoutFrameBytes` returns the larger BGRA size for odd geometry instead
  of an undersized NV12 answer, so a future caller that forgets the even-only
  rule over-allocates rather than tearing frames.
- `build_windows.bat` finds Visual Studio 2026, reuses the generator an existing
  `build-upscaling` cache was created with, and falls back to `Visual Studio 17
  2022` when only a PATH CMake is found rather than guessing the newest
  generator. It runs `ctest` directly, with `--output-on-failure`.
- The contributed `src/ChildProcess.h` was dropped rather than merged: its
  `ChildProcessErrorModeScope` is the same fix as this project's
  `ScopedHardErrorSuppression`, already wired into all seven spawn sites. Its
  header argued the thread error mode alone cannot suppress the invalid-image
  modal; measured here, the mode is consulted - a bad-image `CreateProcessW`
  returns in 0.1-0.2 ms with it set against 6-14 ms without, for garbage files,
  DOS stubs, truncated PEs and machine-type mismatches alike.
- Helper-leak checks count only this process's own children. Counting every
  `ffmpeg.exe` on the machine made them depend on what the rest of the suite was
  doing, and the real ffmpeg other test binaries run moved the baseline
  mid-test; fixture teardown also retries while a just-terminated child still
  has its image mapped.
- Dropped two tests that asserted implementation rather than behaviour: the
  frame-buffer pool's pointer identity, and three ids queried on the wrong
  window. The NV12 guide-equivalence test now uses coloured input, so the BT.709
  weights and the 16/219 range mapping are actually pinned, and asserts that
  motion was detected at all.

## 0.18.0 - 2026-09-10

- The driver is now checked before the neural path runs. Feature 18 is created
  by the driver's own NGX core, so a driver older than the core that knows the
  feature refuses the create before this player is involved: an RTX 3060 Laptop
  on driver 566.14 (DXGI `32.0.15.6614`) logged `feature 18 create failed with
  0xbad00002`, which is `NVSDK_NGX_Result_FAIL_PlatformError`. The locked
  runtime was never the problem there - its fatbin carries `sm_75/86/89/120`,
  Ampere included, and its own architecture refusal returns `0xbad00001`, not
  `0xbad00002`. `ClassifyNeuralDriver` now parses the DXGI driver string
  (`(c % 10) * 10000 + d`, so `32.0.15.6614` is 566.14 and `32.0.16.1664` is
  616.64) and compares it against a 610.47 floor - the lowest driver this
  project has actually rendered on - naming 616.64, the verified one, in the
  message. Startup logs the verdict; a render below the floor is refused with
  "Neural rendering needs a newer NVIDIA driver" and the three numbers, instead
  of spending five seconds in a probe that cannot pass. The community-published
  floor for the same runtime is 616.56.
- The preflight receipt no longer reports a feature-18 result it never read.
  `feature18.ngxCreateResult` was the DLSS Super Resolution carrier's result and
  said `0x00000001` (Success) on the machine above, where feature 18 had failed;
  the real code survived only as text inside an observation line. It is now
  `feature18.carrierCreateResult`, beside a `feature18.createResult` parsed out
  of the observations, and the receipt gained
  `diagnosis:{cause,detail}` (`driverBelowFloor`, `platformRefusal`,
  `architectureUnsupported`, `outOfVideoMemory`, `evidenceIncomplete`,
  `probeFailed`). Receipt schema 1 -> 2. The player's failure text is that
  detail, so `0xbad00002` now reads as a driver or second-consumer problem and
  `0xbad0000d` as a VRAM problem, rather than "did not arm feature 18".
- A failed preflight is no longer repeated on every play and seek. The field log
  ran three ~5 s probes in 65 s, each ending the session and dropping seven
  frames. `NeuralPreflightLatch` keeps one negative verdict per GPU, driver and
  runtime digest; a different driver or runtime re-probes.
- Added an update notice. On startup, and at most once a day, the player asks
  GitHub for the newest release and shows `↑ Update <version>` right-justified
  in the menu bar; opening it goes to the releases page and retires that release
  until the next one ships. **Advanced > Check for updates** forces a check and
  reports the answer either way. It reads `/releases`, not `/releases/latest`:
  every release this project publishes is flagged as a pre-release, and the
  `latest` endpoint answers 404 on such a repository. Drafts are skipped, the
  highest version wins regardless of feed order, and the
  `dlss5-video-player-v0.18.0` tag shape is parsed directly. State lives in
  `[Updates]` in `DLSSVideoPlayer.ini`; `Enabled=0` turns the check off. Checked
  against the live API: the player logged
  `latest release dlss5-video-player-v0.17.2; this build is 0.18.0` and stayed
  quiet, as it should for a build newer than the feed.
- Fixed a hang: a corrupt bundled helper stopped a worker thread on a Windows
  hard-error dialog instead of failing. Seen while running the test suite - the
  placeholder `yt-dlp.exe` a resolver test writes produced a modal
  "cannot start or run due to incompatibility with 64-bit versions of Windows"
  (`ERROR_EXE_MACHINE_TYPE_MISMATCH`), and `CreateProcessW` does not return
  until that dialog is dismissed, so a half-extracted package would have stalled
  acquisition, decode, audio or a render with nothing in the log. Every spawn of
  a bundled executable - ffmpeg, ffprobe, yt-dlp, NeuralWorker and the
  safe-mode relaunch - now runs inside `ScopedHardErrorSuppression`
  (`SetThreadErrorMode(SEM_FAILCRITICALERRORS)`, restored on scope exit), so
  the call fails closed on the error every call site already handles.
  `PolicyTests` finishes in 18 s instead of blocking on the dialog.
- Removed dead code the tests were keeping alive. The in-process bootstrap
  relaunch family went with the isolated helper it belonged to
  (`BootstrapAction`, `DecideBootstrap`, `DecideBootstrapFromObservedUpdate`,
  `BuildBootstrapRelaunchArguments`, and the `addonBootstrapRestarted` flag that
  was set and never read), along with `DecideNeuralOpen`/`NeuralOpenAction` and
  `ExecuteNeuralReplacementSequence`, which the player had stopped routing
  through, and the 150-line YouTube format-availability JSON parser
  (`ParseYouTubeFormatMetadata`, `FormatJsonParser`) that nothing ever
  consulted. `--addon-bootstrap-restarted` is still swallowed by the argument
  parser so an old shortcut still starts the player. Their tests went with them;
  the assertions that covered live behaviour were kept and renamed.
- Removed 2.6 MB of unreferenced files: the raw JSON dumps beside the
  7 September runtime comparison (`analysis.json`, `binary-inventory.json`,
  `crash-events.json`, `visual-metrics.json`), `docs/media/validation.json`, and
  the orphaned `VERIFICATION-2026-09-03-MEDIA.md` record. The prose report that
  those dumps fed is unchanged. `ECOSYSTEM_REVIEW.md` no longer claims the
  driver floor and the fatbin architecture check are missing.

## 0.17.2 - 2026-09-10

- Fixed the neural render wedging at the 60th present, reported on an RTX 4070
  Ti (#4, still failing on 0.17.0) and an RTX PRO 6000 (#3). The renderer
  released and re-created the raw NGX feature on a frame count
  (`DelayedRecreateFrame = 60`), and the offline job's receipt gate re-presents
  one source frame up to 120 times waiting for the add-on to publish a fresh
  feature-18 evaluation. Those two collided: the release landed inside the gate
  and tore down the inline neural worksets whose counter the gate was waiting
  for, so the count never advanced and the job aborted with "A frame was not
  produced by feature 18." The reporter's log shows the add-on's re-arm and its
  `count=60` line in the same instant, then no add-on output at all until
  teardown. Whether it survived was a race, which is why the same build worked
  on an RTX 4080 SUPER. The feature is now created once and kept: NVIDIA's DLSS
  Programming Guide 310.6.0 restricts re-creation to display-resolution, RTX and
  buffer-format changes (S3.2) and requires that no command list referencing the
  feature is in flight when it is released (S5.5), and both sibling projects
  that drive the same add-on treat the warm-up re-create as a one-shot
  workaround for older builds that latch standby on a create they missed,
  disabled outright for the v4.5+ builds that adopt features lazily. A re-hook
  is now only ever explicit, and the job makes that request before capture
  starts - when its own evidence says interception was never armed - so nothing
  releases a live feature while a receipt is outstanding. The evidence baseline
  is read after any such re-hook. Verified on an RTX 4080 SUPER: a live session
  rendered 313/313 frames, `verified=313 failure=none`, past evaluation #300 on
  a single `CreateFeature` and with no re-create line in the worker log.
- DLSS Super Resolution no longer refuses a source that cannot reach the
  requested output. It asked the runtime for the admissible input range at one
  fixed output and gave up when the source fell short, which is what left a
  436x573 photo with no upscaling at all on the RTX 2060 in #2 - and answers #1,
  which asked what resolution the dialog wants. The range is a runtime query,
  not a ratio: the guide documents no maximum upscaling ratio and no fixed
  fraction for the minimum (S3.2.2), so the output is now reduced by exactly the
  shortfall the runtime reported and queried again, at the source's aspect ratio
  as S3.2.2.1 requires, never below the source. Reproduced on an RTX 4080 SUPER,
  where a 1098x1440 output advertises a 549x720 minimum: a 436x572 source used
  to be rejected and now renders 50/50 evaluations at 872x1144. The status text
  says the source is too small for the chosen target instead of reporting DLSS
  as unavailable.
- Source acquisition reports what it is doing. A YouTube neural session copies
  the whole source locally first, and that copy emitted one phase event with no
  counters, so the panel sat still for minutes; the RTX 2060 reporter in #2 read
  it as a hang and cancelled four times, each attempt starting from byte zero.
  ffmpeg is now asked for `-progress pipe:1` and its `total_size`/`out_time`
  are parsed off the pipe it already shares, so the bar tracks the copy and the
  panel names the megabytes and the percentage. The human-readable stderr
  progress line is deliberately not parsed.
- The neural job has a per-phase silence deadline. Acquisition, cache checks,
  preflight, decoding and rendering must keep reporting - 60 s for acquisition,
  120 s for the rest - and a phase that goes quiet for longer fails the job with
  the helper-protocol reason instead of leaving a progress bar running forever.
  Encoding and validation are not watched: ffmpeg's flush and hashing the output
  are legitimately silent.
- Still images can start a neural render from the toolbar. The Neural Rendering
  button was permanently disabled on a photo, because a still cannot run a live
  session and nothing was cached yet, so #2 concluded images were unsupported;
  it now submits the same single-frame job the frame preview uses.
- A live session no longer implies it checked whether the GPU can keep up when
  it has nothing to check with. Only Blackwell and Ada carry a measured pace
  prior, and an absent prior was silently treated as "keeps up", so the promise
  made to #2 did not hold on a 2060. The forecast now reports whether it was
  measured, the log says the pace is unmeasured, and the session status says so
  while it runs.
- Scene-cut detection is debounced. The detector was stateless, so one
  transition fired it 6 times in 12 frames, and every fire wipes the DLSS
  temporal history the reconstruction depends on. The decision is now classified
  by strength; the weak arm (moderate residual plus a collapsed luma histogram)
  is suppressed unless 0.6 s has passed since the last accepted cut, which is
  PySceneDetect's `min_scene_len` default applied as the same hard
  minimum-interval filter, while a strong residual still cuts immediately as
  x264/x265 do inside `min-keyint`. Every frame carries its own verdict -
  strength, suppressed flag, residual, histogram overlap - and playback logs it,
  which it never did before.
- The release workflow attaches a `.sha256` beside the core package it builds,
  and the README's download table describes the assets a release actually
  carries: v0.17.1 published only the 31 MB core zip, while the table promised
  the 308 MB full package and hashes for both. CI cannot build the full package
  because it never fetches the locked runtime, so that one is documented as the
  hand-attached asset it has always been.

## 0.17.1 - 2026-09-10

- Fixed live playback stopping at a segment seam with the modal "out of sync"
  warning. A segment's exclusive end is rebuilt from an integer frame duration,
  so at 30 fps it lands 20 ticks below the next segment's own first pts and the
  shortfall grows by a seam. A playhead inside that sub-frame hole matched no
  segment, and an uncovered timestamp between the render start and the render
  head is read as a producer contract break, so playback stopped about two
  seconds in while the render carried on and finished every frame. It hit two of
  five 1080p30 sessions on an RTX 5090 - the ones whose playback attached
  mid-segment, where the seek leaves the original's timestamps a few ticks below
  the CFR grid. `NeuralSegmentIndex::Append` now closes a hole narrower than one
  frame, and the lookup that picks a segment runs on frame numbers like the
  coverage test beside it already did; a real gap from a rebased relaunch stays
  uncovered. Two tests replay the protocol's own rounding and fail on the old
  code. Four sessions after the fix played to the end of the clip.
- An out-of-sync or decode stop says why in the log: the reason, both frame
  numbers and timestamps, the open segment, the render head and whether the job
  finished. Before this there was nothing but the dialog.
- Re-measured the live-session pace on the RTX 5090 (driver 616.64) after
  0.17.0's pipelined capture and parallel guides: 1080p30 8.36 ms/frame against
  11.89 on 0.16.0, 1440p30 15.44 against 17.15, 4K30 42.0 against 42.87, over
  eight, three and two 30 s sessions of the same clips. 4K did not move because
  that clip is a 6315 kbit/s re-encode whose decode and encode set its pace. The
  4K30 keep-up prompt now appears before the session ("23.5 frames per second,
  0.78x real time"), which the 0.16.0 record could not confirm. 12/12 CTest
  suites and `overall=PASS` from the strict media smoke on that machine. See
  `docs/VERIFICATION-2026-09-10-RTX5090.md`.
- Corrected the speed claims. The README quoted 1080p30 at 10.3 ms/frame on an
  RTX 4080 SUPER and 11.9 on an RTX 5090, which put the slower GPU ahead and
  cited a figure no record supports; the 4080's own measurement is 15.31 ms on
  0.16.0. Every quoted pace now names the build and the machine it came from,
  the docs that said a 5090 keeps up with any source to 4K30 say what the file
  costs instead, and the reference constants in `src/PlaybackTiming.h` say that
  they are a seed for an unmeasured machine rather than current numbers.
- The release workflow uploaded `dist/DLSSVideoPlayer-v0.14.1-core-win64.zip`,
  a name the packager stopped producing three releases ago; it now takes the
  versioned zip it actually builds and fails when there is none.

## 0.17.0 - 2026-09-10

- The neural export is pipelined, ported from ctype-lab's PR #5 with fixes.
  Capture readback rotates through four persistently mapped slots and signals
  a per-slot fence instead of draining the queue; the copy out of the mapped
  slot runs on a worker while the loop decodes, guides and submits the next
  frame; and ffmpeg's stdin is fed from its own thread through an eight-frame
  queue so a 4K frame into a busy encoder no longer stalls the loop. The
  export decoder gains the same four-frame queue thread the player uses.
  Measured on the RTX 4080 SUPER with the 30 s 1080p30 demo, 900 frames:
  render rate 48.6 -> 109.4 frames/s (1.6x -> 3.6x real time), wall 25.8 ->
  15.9 s, capture 11.8 -> 0.02 ms/frame on the loop, guides 4.6 -> 1.5 ms.
  The output is bit-identical to 0.16.0's (PSNR infinite over all 900
  frames), which is the proof that the parallel guides and the pipelined
  capture change nothing but time.
- Not ported: the PR's headless mode, which skipped the backbuffer pass and
  Present once the NGX feature's delayed recreate had gone out. The RenoDX
  add-on performs its feature-18 pass per present, so with it on the export
  ran at the same 109 frames/s but produced DLAA-only frames - 0.46 ms of
  neural GPU time per frame against 5.7 ms, 34.6 dB from the source instead
  of 31.7 - while the receipt still said `frames=900/900 verified=900`. The
  presents cost nothing measurable on this machine. The evidence chain not
  catching a runtime that evaluates DLAA without NR is a gap in its own
  right and is recorded in the roadmap.
- Temporal guide generation fans out over a process-wide worker pool: luma
  downsample, the global translation search, the 2x2-lattice block match,
  the median flow, the depth proxy and the guide-grid pack. Each worker owns a
  semaphore so a two-range split wakes one thread, and the subpixel refine and
  reverse search prune with an exact per-row bound. The PR measured 7.0 -> 1.4
  ms/frame at 1080p, memcmp-identical to the serial reference over 24-frame
  sequences with temporal history. The pool refuses to be entered from two
  threads at once: a second dispatcher runs serially on its own thread rather
  than overwriting the single task slot under running helpers.
- Every stage of the export loop is timed (`Neural export stage cost per
  frame`), source-frame latency and render-loop throughput are separate
  series, and the residual between the stage sum and the loop clock is
  printed, so a slow decoder child, a slow GPU and a swapchain pacing the
  presents no longer look identical from outside.
- Fixed against the PR as submitted: a reader blocked in
  `ReadNextBlocking` now returns `Cancelled` when the decoder closes, instead
  of falling into the raw-pipe poll while the child is being torn down and
  reporting `EndOfStream` (the PR's own regression test failed on this);
  cancelling an export while the encoder feeder is draining its queue - or
  wedged on a pipe ffmpeg is not reading - now releases the blocked write
  (an existing test failed on this); a readback failure during the inline
  drain is classified as the device removal or stall it was, not as the
  previous frame's failure; the swapchain keeps three backbuffers while the
  allocator count goes to six, so the visible player does not spend ~+100 MB
  of VRAM at 4K on presents DXGI's latency limit would never queue;
  `RawVideoEncoder::WriteFrame` re-checks the stop after every write, because
  terminating the child under a blocked `WriteFile` completes that write as a
  success and a single remaining chunk reported `None` for a frame nobody
  read; and the unused `EncoderPixelFormat::Rgba` and `CaptureRenderedFrame`
  were deleted. The PR's "shutdown deadlock fix" repaired a regression from
  its own first commit; `main` never had it.

## 0.16.0 - 2026-09-09

- NGX's own diagnostics reach `DLSSVideoPlayer.log`. `DLSSBackend::Initialize`
  now hands NGX a `LoggingInfo` callback; lines that name an error, failure,
  warning or unsupported condition are copied beside the player's own as
  `[NGX feature N]`, so why a feature refused to create is read in one file at
  the moment it happened. The complete stream still goes to `ngx_logs/`; the
  ~170 startup lines each worker emits are not copied.
- `tools/package_release.ps1` finds the locked runtime the fetch script staged.
  The fetch stages each file under the lock's `sourceName`, so the universal
  NR runtime sits in `external/runtime` as `nvngx_dlssnr_310.8.SF-v2.dll`;
  the packager looked only for the `destination` name and refused with
  "Locked input is missing" on a tree that `stage_runtime.ps1` had just
  verified. It now resolves every locked input the way the stager does.
- `tools/fetch_neural_runtime.ps1` fetches the whole locked neural runtime.
  Every file in `packaging/runtime-lock.json` comes from a public release, so
  the script downloads the five source archives, checks each archive's
  SHA-256, extracts the locked members (bsdtar for the ReShade installer,
  whose ZIP directory .NET rejects), checks each against the lock, stages
  them in `external/runtime` and runs the full validator. Already-matching
  files are not downloaded again. `build_windows.bat` now runs it, so a fresh
  checkout builds the complete experimental layout with one command; a fresh
  directory fetched and verified all 12 files in 10.8 s. Fetching for a local
  build does not change the redistribution status of the combined set.
- Verified on an RTX 5090 with the universal runtime: strict smoke `PASS`,
  the previously failing 1080p live session at 869/869 verified frames and
  11.89 ms/frame, and full renders at 1440p (17.15 ms/frame) and 4K
  (42.87 ms/frame). See `docs/VERIFICATION-2026-09-09-RTX5090.md`.
- The keep-up forecast now predicts from what this GPU measured at each
  geometry, not one scalar. The 5090 run showed the scalar assumption wrong:
  0.95x the reference at 1080p, 1.04x at 1440p, 1.53x at 4K, so a 1080p-only
  scalar predicted 26.6 ms for 4K30, started the session without warning and
  dropped 848 of 869 frames while the real cost was 42.9 ms. The player keeps
  one measured pace per source geometry (`[NeuralPace] Samples=WxH:ms;...`),
  uses an exact match as is, fits its own fixed + per-megapixel line from two
  or more geometries, and extrapolates from a single sample by the larger of
  the reference shape and a purely proportional cost. With only its 1080p
  sample the RTX 4080 SUPER now warns before 4K30: "17.2 frames per second,
  0.57x real time".
- Neural rendering on every RTX generation. The locked neural runtime is now
  ShortFuse's universal `310.8.SF-v2` build (`nvngx_dlssnr.dll`, SHA-256
  `6EB209E7…3927`), which extends the leaked 310.8 runtime to Turing, Ampere,
  Ada and Blackwell; the previous lock was the RTX 40-targeted `310.8.0-RTX40`
  build, which had no Turing or Ampere code. The GPU policy enables the add-on
  for GeForce RTX 20/30/40/50 and for RTX-branded workstation and laptop parts,
  and stops fail-closing on the product name: feature 18's own creation and the
  strict evidence chain still refuse a GPU that cannot run it. The generation
  label in the cache identity gains `rtx20`, `rtx30` and `rtx`. Verified on an
  RTX 4080 SUPER (driver 610.47): the strict GPU smoke passes on photo, GIF and
  video with both the old and the new runtime at the same per-frame cost, and a
  30 s 1080p30 live session rendered 798/798 verified frames with zero dropped
  presents. An RTX 5090 then confirmed the same runtime (below); Turing and
  Ampere have no hardware verification in this project yet.
- Fixed the render wedging right after preroll on an RTX 4080. The automatic
  NGX feature recreate that arms the add-on's capture fired on the first
  captured frame, sixty frames in, and released the DLSS feature while up to
  two earlier frames' evaluations were still on the GPU. The add-on hooks that
  release and tears down its NR worksets, and the queue never came back; every
  1080p live session on the 4080 failed with `retry-exhausted frames=0/0`
  after exactly 2 s, twice in two attempts. The renderer now drains the queue
  before releasing a feature. The same session then completed end to end.
- Frame waits during rendering get their own 20 s budget instead of sharing
  the 2 s teardown budget. The short budget classified any GPU that needs more
  than 2 s for three pipelined frames as a stall, which is exactly where a
  Turing or Ampere part lands at 1080p. Teardown keeps 2 s, and device removal
  still surfaces immediately through the fence's `UINT64_MAX` sentinel.
- The live-session forecast speaks for this machine's GPU. The 7.35 ms +
  2.50 ms/megapixel model was measured on an RTX 5090 and was applied to every
  GPU as if it were one. The player now measures the steady-state pace of each
  session from segment arrivals - the method the reference numbers used - and
  saves it per GPU in `DLSSVideoPlayer.ini` (`[NeuralPace]`). Until a machine
  has measured itself, Ada assumes 1.22x the reference cost (the RTX 4080
  SUPER measured 15.31 ms/frame at 1080p over 738 frames), Blackwell 1.0x,
  and other generations make no forecast at all rather than a wrong one.
- The runtime staging script compares file versions numerically, like the
  player does, because the universal runtime's version string (`310.8.SF.0`)
  is not what its numeric version block (`310.8.2.0`) says.

## 0.15.0 - 2026-09-09

- Turn neural rendering on while watching. The toggle starts a render at the
  playhead (to the Out marker when the playhead is inside a marked range, else
  to the end of the source) and playback follows the render head: a buffering
  panel over the current frame collects a 4 s lead, playback resumes on the
  rendered frames, and it rebuffers if the playhead catches up. The timeline's
  teal lane grows with the head, seeks clamp to it, and a finished session is
  concatenated into the ordinary cache entry.
- Turning a session off and on again resumes instead of re-rendering. Stopping
  used to delete every segment it had produced, so the next toggle paid the job
  startup again and re-rendered frames that had existed seconds earlier. The
  segments are kept and adopted when the source, neural settings and guides
  still match and the playhead is inside their coverage; the new job then starts
  at the render head, and each job writes its own subdirectory so a relaunch
  discards only its own output. Measured on a 64 s source after rendering 34 s
  of coverage: the resumed session started at 39.47 s rather than the playhead,
  re-rendered nothing (21 segments, no overlapping spans), and playback attached
  0.74 s after the toggle with 34 s buffered, against 12.01 s on the first start.
- Color strength and the render preset are gone from the neural settings dialog.
  Measured one control at a time against the pinned runtime, six of the eight
  model parameters and both guides change the output - intensity moves 84.75% of
  bytes, local tone 66.45%, local structure 53.41%, style 49.31%, the
  motion-vector guide 39.79%, depth 37.92%, skin structure 36.50% and the
  automatic mask 36.08% - while color strength and the preset move nothing, on
  four preset pairs and two colour baselines. Driving the runtime INI directly
  shows the add-on echoing `preset=1` and `preset=3` back, so the hint reaches
  NGX and the model ignores it. Each change still cost a 10.6 s re-render for
  byte-identical output. Both keys stay in DLSSVideoPlayer.ini and in the render
  identity, so runtime-comparison work can still drive them and a runtime that
  does honour them cannot be served a stale cache entry.
- Face comparisons from three more trailers - Hellblade II, Cyberpunk 2077
  Phantom Liberty and Mafia: The Old Country - beside the existing Witcher IV
  figure. Both halves of each are the identical source pixels of the identical
  frame with no scaling or retouching, and the right half is a real render from
  the shipping worker rather than a mock-up. The figure script now takes
  `--frame`, `--neural-frame`, `--crop` and a wrapped `--caption`, and crops are
  constrained to the picture area so a letterboxed trailer no longer contributes
  black bars.
- A session that cannot keep up says so before it starts. Rendering costs
  7.35 ms per frame plus 2.50 ms per megapixel on an RTX 5090 (fitted to 12.50,
  16.60 and 28.07 ms/frame measured at 1080p, 1440p and 4K), so the player can
  predict the rate from the source's geometry and frame rate: it warns with the
  numbers and asks before starting a session on 4K60 or 8K, and reports the rate
  a running session is actually achieving when that falls behind. Everything up
  to 4K30 keeps up - measured 1.165x real time in the player on a 40 s 4K30
  source, with no rebuffering.
- Decoding no longer answers every 4 MiB of a frame with a sleep. The read loop
  drains the decoder pipe, which cuts steady-state cost from 15.57 to 12.50
  ms/frame at 1080p, 46.83 to 16.60 at 1440p and 109.3 to 28.07 at 4K. ffmpeg
  alone decodes those files in 2.5 to 8.2 ms/frame, so the transport had been
  costing six to thirteen times the decode.
- Short forward seeks keep the running decoder instead of restarting it. Frame
  stepping and scrubbing under 0.15 s go from 265 ms to 0.3-1.0 ms, every
  restart seek is ~30 ms cheaper because the old child is killed after the new
  one is spawned, and SeekSeconds itself returns in 1.4-8.2 ms instead of 44.8,
  which matters because the player calls it from the UI thread. Seeks are now
  snapped to the frame grid, which also removes an ambiguity where an unaligned
  target could decode either of two neighbouring frames.
- Motion vectors now help instead of hurting. Acceptance is by how much the
  winning displacement beats standing still, ambiguous cells are verified by a
  reverse search, and the median filter is a confidence-weighted vector median
  over accepted cells only. False motion on the cuts clip falls from 60.8% to
  3.7% of cells, temporal stability on static text beats even the mv-off
  ablation (0.3292 against 0.3342 and the old 0.4101), PSNR-Y recovers 0.297 dB,
  and the guide pass costs 3.09 ms instead of 6.66.
- The mask guide is gone. Neural rendering ignores it even when forced to all
  ones, the preset hints make no difference either, and on the upscaling path
  mask on versus off is byte-identical while motion vectors change 6.85% of
  bytes. Two of its three NGX parameter names were Ray Reconstruction inputs
  that SuperSampling never reads. The checkbox, the debug view, the R8 texture
  and the bindings are removed; the cache key schema is bumped accordingly.
- Rendering behind playback no longer stalls on its own encoder. Segment
  rotation cost 141 ms of render time each, mostly because a freshly spawned
  ffmpeg does not drain stdin for ~110 ms; rotation and the writes now happen off
  the render thread, and segmented rendering is slightly faster than single-file.
- Capturing a frame no longer swizzles on the CPU or drains the whole GPU queue:
  6.85 to 5.73 ms, bit-identical output. The render loop also decodes the next
  frame while the current one is on the GPU, which cuts frame p95 from 25.99 to
  17.57 ms.
- Preview neural settings on the paused frame. Moving a slider re-renders the
  frame the player is paused on 700 ms after the sliders settle and shows the
  result in its place; the toggle reads "Settings preview" while that frame is
  displayed. Repeating a setting is a cache hit (~2.5 s instead of ~12 s).
- **Apply** in the neural settings dialog now applies: it restarts an active
  session or re-previews the paused frame instead of launching a whole-video
  re-render. Writing files moved to **Convert & save**: convert the marked
  clip, convert the whole video, save the converted video, cancel saving.
  Saving refuses an entry rendered with settings that have since changed and
  offers to convert that range again, so a saved file matches the selection.

- Open media without rendering it first. A YouTube URL plays from its stream as
  soon as it resolves (about 6 seconds here), a local file replays a validated
  cache entry when one exists, and otherwise the original plays immediately;
  In/Out markers, the frame and clip previews, **Render marked range** and
  **Render whole video** decide what is rendered.
- Acquire a streamed source once, into the cache, and start it when a range is
  marked rather than when the video is opened or the render is requested: the
  download runs in the background while playback continues, and the render waits
  for that copy instead of starting a second one. The page URL is re-resolved
  once when the stream URLs have expired. A source played from its cached copy
  seeks locally instead of re-opening the network stream, and end of file ends
  playback.
- Make the marked range unmissable: the timeline is taller, the selection is a
  solid violet block across the track between DPI-scaled green/orange In and Out
  ticks that reach past it, cached neural coverage moved to a teal stripe along
  the bottom, and the status line leads with the In/Out timecodes.
- Seek in about half the time and scrub with a live picture. A hardware decode
  path that cannot start is remembered per codec for the rest of the run, so
  restarts stop relaunching ffmpeg on paths that cannot work for that source
  while codecs the GPU does handle keep using it; a seek to the container end
  stops at the last frame instead of paying for a second restart; and dragging
  the timeline now decodes the frame under the cursor (throttled, local and
  cached sources) instead of showing nothing until the mouse is released. The
  audio helper is no longer respawned per scrub step, the play state survives the
  drag - including one interrupted by losing mouse capture - and a release that
  lands on the frame already decoded skips the duplicate seek. Measured on a
  1080p FFV1 clip: 820-1290 ms per seek before, 336-401 ms after.
- Keep In/Out markers inside the source. A rounded duration and a rounded frame
  rate can grid a frame just inside the end that the source never emits (a
  30.03 s clip at 59.94 fps grids frame 1800 at 300299799 against a 300300000
  duration); marking there asked the helper to render a range with nothing to
  decode, which failed with `frames=0/0` and "Feature 18 could not be primed
  from the source". A frame now counts only when it starts at least half a frame
  before the end, In stops at the last emitted frame, Out on that frame means
  the source end so the final frame is renderable, and a range that names no
  frame is refused before the render starts.

- Verify the staged neural runtime against the embedded packaging lock and run a
  Feature 18 preflight in the isolated helper before every render; each cache
  entry now carries `receipt.json` (GPU, driver, ReShade/RenoDX/DLSS-NR versions,
  module hashes, feature-18 observations, effective settings, timing) hashed
  into a schema-4 manifest, summarized in one log line.
- Allow only one render at a time to use the shared experimental runtime: a job
  leases the runtime directory from the settings write until the helper exits,
  so a second player instance can no longer swap its neural settings or proxy
  log into another render; it is refused with a clear message instead. Feature-18
  evidence is read from the log of the session that produced it, never from a
  stale or foreign one.
- Attach a frame identity (frame number, timestamp, source generation, history
  generation, job id, reset reason) to every decoded frame, guide and neural
  result; reject mismatches; log every temporal reset with its reason. Cuts
  now need both a high post-alignment residual and a low luma-histogram overlap,
  so fast pans no longer reset history while real cuts always do.
- Classify neural render failures (GPU stall, device removed, worker crash,
  retry exhausted, preflight, identity, protocol) with explicit player states,
  retry the exact frame a bounded number of times, relaunch a crashed helper
  from frame zero at most once, and pause/resume a render with Space. A failed
  frame is never omitted from the output.
- Add In/Out markers, exact timecode entry, single-frame and 4 s neural
  previews with temporal pre-roll, and range renders whose cache entries,
  synchronized playback and exports (audio/subtitles trimmed) follow the range.
- Add Blend, Split, Wipe and Zoom comparison presentation against the original
  member of the synchronized pair, and a Neural settings dialog with separate
  Intensity, Structure, Tone, Skin, Color, Preset, Style, Auto-mask and guide
  (motion/depth/mask) controls that become part of the cache identity.
- Add a repeatable quality benchmark (`tools/benchmark`): synthetic corpus,
  worker driver with guide/setting ablation and two-pass chaining, flicker,
  color-shift, PSNR/SSIM, OCR and face-consistency metrics, blind A/B pairs,
  plus a documented reference run (`docs/BENCHMARK.md`).

## 0.14.1 - 2026-09-03

- Make the fullscreen lifecycle regression portable across narrow and high-DPI
  Windows test desktops so the release gate validates supported window sizes.

## 0.14.0 - 2026-09-03

- Replace the example menu with six official trailers featuring human characters,
  all under three minutes, under **File > Game trailers**.
- Open and process photos (PNG/JPEG/BMP/TIFF/static WebP) and animated GIFs.
  Export processed media to PNG, JPEG, GIF, MP4 or MKV, with atomic publication
  and cancellation. Single photos use one captured frame; GIF processing keeps
  centisecond timing and GIF export uses viewer-compatible 20 ms frame delays.
- Preserve odd photo dimensions during neural caching and MP4 export instead of
  allowing NVENC or YUV 4:2:0 encoding to pad them to even dimensions.
- Prefer a writable `cache/v1` folder beside the EXE, falling back to LocalAppData.
  Automatic storage follows portable moves; explicit custom locations remain supported.
- Hide the menu and controls on fullscreen entry, reveal them on mouse movement,
  and hide them after 2.5 seconds idle while preserving keyboard and drag interaction.
- Honor YouTube's advertised stream-availability time before opening resolved
  media URLs, preventing premature HTTP 403 failures. Preserve cancellation and
  the resolver timeout, including fractional availability timestamps.
- Replace the demonstration and comparison screenshots with The Witcher IV's
  village sequence, with no sexual content in the 30-second clip; include the
  reproducible Remotion edit and capture provenance.

## 0.13.0 - 2026-09-03

- Replaced the example categories with five official upcoming-game videos.
- Added persistent recent-five history with validated source/render cache reuse
  and removal of displaced, unreferenced tracked cache entries.
- Included canonical neural settings snapshots in render cache identity and
  rejected publication when settings change during rendering.
- Persisted playback, comparison, upscaling and YouTube-quality preferences.
- Select the highest advertised video bitrate at each chosen YouTube resolution,
  across codecs and containers; Auto retains its 1080p-first behavior. Refresh
  older source caches once so they cannot bypass the new selection policy.
- Added cancellable stream-copy MKV export with source audio, compatible
  subtitles, attachments, metadata and chapters; existing outputs are protected.
- Resolved the physical writable cache root under Windows package redirection
  while retaining strict cache ownership checks.
- Remembered the resolved cache location across launches; example and pasted-URL
  opens now reuse matching recent sources before resolving YouTube again.
- Fixed truncated YouTube acquisition: preserve video beyond a shorter audio
  stream, retry interrupted HTTP reads, and verify decoded video duration against
  YouTube metadata before rendering. Compare neural timing against the source
  video track instead of a longer audio tail. Older source caches are replaced once.
- Removed legacy quality options and refreshed usage documentation and screenshots.
- Added animated loading/cache-check indicators, indeterminate progress during
  setup, and real frame percentages during neural rendering. Animation stops
  during ordinary playback and respects Windows' client-area animation setting.
- Isolated the offline neural renderer in `neural-runtime/NeuralWorker.exe` so
  playback can use independent NVIDIA NGX Super Resolution.
- Added runtime DLSS Upscaling, off by default, with 1440p/default and 2160p
  outputs. Preserve source dimensions, cached media, comparison and playback state.
- Keep Neural Rendering on by default and Frame Generation unavailable.
- Reject old root-level proxy layouts; extract the whole build into a new folder.
- Added process-protocol, first-frame SR, output-size and opt-in GPU smoke tests.

## 0.12.0 - 2026-09-01

- Replaced the ambiguous DLSS toggle with synchronized Neural Rendering,
  DLSS Upscaling, and Frame Generation controls. Neural Rendering starts
  requested on for cached comparison; Upscaling is disabled for the neural-hook
  conflict and Frame Generation is unavailable without an FG backend.
- Reuse validated cached videos with content-hash and header checks, avoiding
  repeat full-video decoding; show a distinct cache-checking status.
- Fixed cached-video seeks treating decoder startup as failure; wait for both
  frames and clamp end-of-timeline clicks to the last playable frame.
- Fixed garbled Unicode punctuation in neural-render progress and playback text
  by compiling C++ sources explicitly as UTF-8.
- Restored Open/YouTube/Examples menu availability after neural rendering
  succeeds, fails, is cancelled, or cannot start.
- Added public non-DRM YouTube playback through pinned yt-dlp and Deno helpers.
- Added six curated game/anime examples with availability-change handling.
- Added English-only runtime/UI policy and simplified RTX 40/RTX 50 defaults.
- Added the optional experimental RenoDX neural-rendering path, enabled by
  default with a safe-mode escape hatch.
- Added pinned runtime hashes and fail-closed staging for the supplied DLSS
  310.8, ReShade, RenoDX, and Streamline binary set.
- Release binaries remain experimental: the modified neural DLL has an invalid
  Authenticode signature, RTX 50 hardware was not tested, and redistribution
  permission for the supplied runtime set is unresolved.

## 0.11.0 - 2026-08-29

- Added live post-DLSS image adjustments: brightness, contrast, saturation, gamma, temperature and tint.
- Added a dedicated Image Adjustments tool window and toolbar/menu entry.
- Added `Ctrl+Alt+C` overlay-safe global shortcut for Image Adjustments.
- Added paused-frame presentation heartbeat so ReShade remains responsive while playback is frozen without advancing video or NGX history.
- Prevented GDI background erases over the D3D12 render child and limited mouse-hover invalidation to the control bar to address surface flashing.
- Added `WS_CLIPCHILDREN` / `WS_CLIPSIBLINGS` window composition changes for the video surface.
- Preserved image settings and language independently in `DLSSVideoPlayer.ini`.
- Restructured documentation and repository metadata for public GitHub use.

## 0.10.0 - 2026-08-29

- Added Windows-level transport hotkeys that continue working while ReShade captures normal input.
- Reworked D3D12 submission into a three-frame ring instead of flushing the GPU every frame.
- Added realtime frame-drop recovery and lower-bandwidth decode policy for high-resolution video.

## 0.9.0 - 2026-08-29

- Added idle startup window, drag-and-drop, modern file picker and language packs.
- Reworked seek/audio lifetime to avoid seek-time handle races.
- Added black aspect-ratio viewport.

## 0.8.0 - 2026-08-29

- Completed raw D3D12 NGX `EvaluateFeature_C` path with reconstructed motion, depth and temporal mask resources.
