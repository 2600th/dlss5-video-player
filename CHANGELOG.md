# Changelog

## Unreleased

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
