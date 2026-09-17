# Changelog

## Unreleased

- **Frame generation ships, as a conversion.** **DLSS > Generate frames** plans
  a multiple from the display, writes a new file at that rate through
  `FrameGenerationPass`, reports the percentage and the time left on the status
  line, and switches playback to the result at the position you were watching -
  the shape chosen because live pacing would have to interleave generated frames
  into the playback clock, which also owns audio sync, dropped-frame accounting
  and seeking.
  The pass does not upscale. It generates at the input's own resolution and the
  player's runtime SR then runs live on the converted file, so the shipped order
  is frame generation first, Super Resolution at present time - the opposite of
  NVIDIA's in-engine order, and deliberately: generating at 720p is roughly nine
  times cheaper than at 4K, and the player-side SR applies to any file it opens.
  An earlier version of this entry cited NVIDIA's order as the justification,
  which described something this code does not do.
  Verified end to end in the player on an RTX 5090 / 616.64 with a 120 Hz panel:
  a 90-second 1280x720 30 fps clip planned 4x and converted in 30 s to 10800
  frames (8097 generated, 8098 evaluates) from 2700 source frames, carrying its
  AAC track, and the player reopened the result at 120 fps with the duration
  unchanged.
- **The ceiling is five generated frames per source frame, and the cap that
  shipped at one was wrong twice over.** `kPhaseVerifiedMultiFrameCount` is now
  5, which is also this RTX 5090's `DLSSG.MultiFrameCountMax`, so 2x through 6x
  are admissible and the plan still takes the smaller of the two numbers.
  Measured through the shipped pass on a 1280x720 30 fps FFV1 clip carrying a
  200x200 textured patch that moves exactly 40 px per source frame, reading each
  output frame's position from its brightness centroid: at 4x the intermediates
  land at 0.191 / 0.474 / 0.707 of the interval against an ideal 0.250 / 0.500 /
  0.750, at 6x at 0.157 / 0.281 / 0.474 / 0.628 / 0.809 against 0.167 through
  0.833, and every multiple from 2x to 6x is monotonic, strictly inside the pair
  and evenly spaced to within 0.11 of one interval.
  Both of the measurements behind the old cap are refuted here. The first used a
  flat WHITE SQUARE as the moving probe: a featureless region has no interior
  detail to localise, so the generated frame is close to a blend of the pair and
  its centroid is pulled to the midpoint - the same runs measure 0.482 / 0.552 /
  0.735 with the square beside 0.191 / 0.474 / 0.707 with texture, which is
  where "the motion arrives as 48/7/19/26 percent of the interval" came from.
  The second claimed a 240-frame 4x conversion produced "240 unique frames" from
  `framemd5` over a lossy re-encode, where identical inputs still hash
  differently; that instrument cannot answer the question it was asked.
  What the cap restores: 24 fps film reaches exactly 120 fps at 5x on a 120 Hz
  panel, pulldown gone, and 30 fps reaches 120 at 4x.
- **A real defect the old measurement was hiding: the reset evaluate carried the
  wrong count.** The pass hands its history-establishing evaluate - the one at
  the first frame and after every decoder discontinuity - `multiFrameCount = 1`,
  not the pair's own count. Driving it with the pair's count made this runtime
  return the newer source frame byte for byte at every index: a 4x conversion
  measured phases 1.002 / 1.002 / 1.002 in the first interval after each reset
  and -0.634 / -0.107 / 0.565 in the second, recovering only from the third.
  `DlssgEvaluateSmoke` keeps that shape as a named negative control, and
  `FrameGenerationSmoke` now measures the phases of the first interval on every
  run, so neither failure can return unnoticed. `DLSSG.BackbufferFrameID` is
  also declared now, one id per decoded source frame, with every index inside a
  pair carrying the newer frame's id.
- Converted files carry the original's audio, subtitles and chapters by stream
  copy, and they come from the ORIGINAL even when the frames come from the
  neural render. Without the copy the conversion produced a silent film, which
  the frame-count and duration assertions could not see. Then the neural path
  produced one anyway: this project writes neural carriers video-only, so muxing
  from the file the frames came from carried nothing and the pass's own audio
  check passed by comparing zero streams against zero. `streamSource` is a
  separate field for exactly that reason, and a smoke registration converts a
  video-only carrier with a separate audio source and fails on
  `outputAudioStreams=0`. The generated video's duration equals the source's,
  which is what keeps a plain copy correct; a silent source stays silent without
  failing.
- The conversion reads the neural render when that is what you are watching, and
  only when that render covers the WHOLE source and matches the settings on
  screen. A range render covers its range alone, so converting it returned a
  clip-length file that was then adopted under the film's title; a stale render
  is not the picture on screen. Those are the two cases the cached export
  already refuses, for the same two reasons.
- The converted file is reachable after the dialog closes. It goes into the
  neural cache rather than beside your source - the first version dropped a
  large MKV into the user's library with no save dialog, landed in the
  acquired-copy directory for a network source and simply failed on a read-only
  share - and **DLSS > Show converted file** opens it with the file selected.
  `NeuralCacheManager::Clear` now removes `frame-generation/` too: it was
  excluded while `SizeBytes` counted the whole root, so the prompt offered to
  free bytes it kept and generated files accumulated with nothing able to delete
  them.
- A conversion no longer hijacks playback, and no longer loses your place. The
  result is adopted only when the file it was made from is still the one on
  screen - compared against both of the current file's carriers rather than the
  view-dependent pick, so pressing `D` while waiting is not mistaken for leaving
  - and playback resumes at the position you were at, because the conversion
  preserves duration exactly. The converted file is not recorded in Recent
  videos: it is a derived carrier whose path would dangle the moment the cache
  is cleared.
- Two refusals that could never fire now can. `PlannedFrameGeneration` declared
  every source constant-frame-rate and passed the decoder's 30 fps fallback as
  if it had been read off the file, so `VariableFrameRate` and
  `UnknownSourceRate` were unreachable - a variable-rate phone recording sailed
  through and the pass emitted `sourceFrames * multiplier` at a rate the file
  never had, which the audio it now carries would drift against. `VideoDecoder`
  keeps both probed rates and answers `FrameRateKnown()` and
  `ConstantFrameRate()` (`avg_frame_rate` against `r_frame_rate`, 0.5%
  tolerance); measured 30/30 on a CFR clip and 37.25/50 on a VFR one.
- **Measured hazard, recorded so it is not tried again:** the runtime's
  admission is probed inside the click that needs it, never in the background.
  Probing at load - a second D3D12 device and a second NGX feature create, 0.2 s
  after the renderer armed its own deferred SuperSampling create - froze
  presentation on the 19th frame of a 600-frame clip while the decoder read on
  to the end. A conversion started from a settled playing session was measured
  safe: the probe, the conversion and a live SR feature coexisted, and SR was
  re-created on the converted file afterwards.
- One state machine now feeds the menu item, the toolbar pill and the status
  line, so they cannot disagree. Before this the menu item stayed enabled while
  the pill greyed out on a seek, "ready" was claimed on hardware nobody had
  asked, and a busy player read identically to a video that can never be
  converted. Every refusal reports its own sentence instead of the log slug
  (`no-even-multiple`), the pill reads **Cancel** while a conversion runs and
  cancels, `Esc` cancels like it does for every other long job, the cancel no
  longer joins the worker on the UI thread (the window stopped painting until
  the pass noticed), the status line leads with the percentage and the time left
  and puts the cancel route before the counters it used to hide behind, and the
  confirmation is Yes/No with No focused rather than one stray Enter from
  minutes of GPU work. Frame generation also has its own toolbar icon: it was a
  third `Sparkles` beside the two real toggles, and at narrow widths three
  identical pills.
- Every frame-generation string lives in `Localization.h`. The whole feature was
  hard-coded literals - eight of them in `main.cpp` alone - so it could not be
  translated and its wording could not be reviewed in one place. Dialogs no
  longer carry hex `NVSDK_NGX_Result` values or NGX key names either: the
  refusal a user with an unsupported GPU sees is a sentence plus the driver ask,
  and the runtime's own answer goes to the log where it already was.
- The three DLSS features are named one way each. The status line reported the
  toggle the user had just flipped as "DLSS SR" one line below a menu item and a
  pill that both call it **DLSS Upscaling**, and frame generation was "Generate
  frames", "Frame Generation" and "FG" in one screenshot.
- The status line fits. It was eleven segments, over 700 characters once any
  notice was prepended, drawn into one ellipsised row about 120 characters wide,
  so the frame rate and both feature states were permanently past the ellipsis.
  It prints what differs now: one geometry unless the pipeline changes it, the
  DLSS input size dropped because it only ever repeated the source or the
  output, and a dropped-frame count only when frames were dropped.
- The player's first window fits the display. Its default 1440x880 client is a
  1440x939 window, and on any work area shorter than that Windows placed the
  bottom off screen - which is where this player draws its status line and its
  entire seek bar. It now shrinks into the work area instead.
- A conversion and an export can no longer run at once, in either direction, and
  neither can a conversion and a range render: frame generation already refused
  to start while an export ran, but not the reverse, so both competed for the
  GPU and the helper directory while the export's status line hid the
  conversion's progress. Clearing the neural cache mid conversion is refused for
  the same reason - it deletes the staging file and the output from under the
  running pass. The export's cancel hint also names the menu it is actually in
  (**DLSS > Convert & save > Cancel saving**, not File).
- One muxer. `MuxVideoWithSourceStreams` was a second MKV stream-copy command
  beside `BuildCachedExportArguments`; it is now an adapter over
  `CachedVideoExporter`, which brings the exclusively created staging file and
  the atomic rename with it. The pass requires its output to be named `.mkv`,
  because that exporter picks the container from the extension and an `.mp4`
  name would re-encode the frames it just generated.
- `tools/verification/capture-window.ps1` captured the CLIENT rect while
  `PrintWindow` renders the whole window, so every screenshot it has ever taken
  cut the bottom of the window off - the status line and the seek bar. Two UI
  defects were investigated from screenshots that could not show them.
- `nvngx_dlssg.dll` ships beside the player and is held to the pinned SDK's own
  bytes by `tools/verify_package.ps1`. It is deliberately NOT a
  `packaging/runtime-lock.json` entry: that lock is the render helper's runtime
  set, every entry must exist under `neural-runtime/`, and the helper never
  creates the Frame Generation feature. Adding it there was tried and reverted
  after the first launch of a fresh build logged "Neural pre-render failed: The
  configured neural runtime is incomplete." - the lock also feeds
  `runtimeDigest`, so the entry would have retired every cached render and
  refused neural rendering on every existing install until its helper directory
  was re-staged.
- The upscaling output is chosen from the display instead of a fixed pick.
  **Auto** is the new default: it takes the largest rung the monitor can
  actually scan out (`AutoUpscaleTargetHeight`, read from the adapter's current
  mode rather than `rcMonitor`, which a scaled display reports in logical
  pixels), and `UpscalingTarget`'s existing `grows` guard still decides whether
  that rung is an upscale at all. A 1440p source on a 4K panel now reaches
  2160p, which the old fixed 1440p default refused as "source meets output"; a
  4K source still reports exactly that and stays off. Verified on an RTX 5090 /
  616.64 with a 1920x1080 panel: a 1280x720 source logged `Playback SR enabled:
  1280x720 -> 1920x1080`, and a 1920x1080 source left the control reading
  "Unavailable" with no feature created.
- Added a 1080p rung. The two-rung menu could only serve a 1080p panel by
  rendering 1440 lines for it - 78% more pixels than it can show, and the DLSS
  evaluate is charged per output pixel. Auto never selects a rung above the
  panel for the same reason.
- The refusal now says which refusal it is: a panel below the smallest rung
  reads "display below 1080 lines" instead of borrowing "source meets output",
  which sent a viewer looking for a broken toggle.
- `UpscaleAuto` is a new `[Playback]` key, separate from `UpscaleHeight`. Every
  earlier version persisted `UpscaleHeight` on every save, so a stored 1440 is
  the old default rather than a choice; keying Auto off that value would have
  denied Auto to every existing install. Absence of `UpscaleAuto` identifies
  those files and means Auto.
  One consequence for ini-driven harnesses: a profile that pins a rung must now
  write `UpscaleAuto=0` beside `UpscaleHeight`, or Auto takes over and a clip
  matching the runner's panel legitimately reports no upscale. The dated
  profiles under `docs/measurements/` are records of past runs and keep the keys
  they were run with.
- Settled whether Frame Generation is reachable at all from this player, which
  had been assumed impossible without Streamline. It is not: raw
  `NVSDK_NGX_D3D12_CreateFeature(..., NVSDK_NGX_Feature_FrameGeneration, ...)`
  returns `NVSDK_NGX_Result_Success` on an RTX 5090 / driver 616.64 at
  1920x1080 `B8G8R8A8_UNORM` with no `sl.*` module in the process, and
  `DLSSG.MultiFrameCountMax` reads 5. `DLSSGBackend::Probe` is that
  measurement, and `DlssgProbeSmoke` (gpu label) is how it is re-taken.
  The one hard prerequisite, found by negative control run twice: `nvngx_dlssg.dll`
  must be resolvable beside the executable. Without it the identical create is
  refused with `0xbad0000b` and the capability block reads
  `FrameGeneration.Available=0` / `FeatureInitResult=0xbad00004`, even though
  NGX locates and logs the driver-store fallback snippet. HAGS is not a gate
  here: `HwSchMode` is absent from the registry on the test machine and the
  runtime admitted the feature anyway, so the probe reports an absent value as
  absent rather than as off and never refuses on it.
  No evaluate, presentation or pacing path exists yet, so the Frame Generation
  control stays unavailable; this commit buys the go/no-go, not the feature.
- The same path also produces frames, which admission does not imply. Measured
  with a synthetic pair whose answer is known in advance - a 200x200 square
  translated exactly +200 px between two 1920x1080 frames - the raw evaluate
  writes a real intermediate frame: all 2,073,600 output pixels overwritten
  from a sentinel fill, the square's horizontal centroid at 812.7 against 699.5
  and 899.5 in the inputs, differing from both, soft-edged across 736..894. It
  is not a cross-fade, which would have spanned 600..999. It lands 13.2 px past
  the 799.5 midpoint, biased toward the newer frame, which is a timing fact any
  pacing work has to account for rather than assume away.
- Recorded a negative result that constrains the design: the tagged
  `DLSSG.MVecs` do not drive that output on this runtime. The same pair
  evaluated with motion in backbuffer pixels, in normalised screen units, and
  with the motion buffer deliberately zeroed - claiming nothing moved while the
  colour pair jumps 200 px - produced the same frame all three times (centroids
  812.73 / 812.73 / 812.79, mean channel difference between runs 0.00 and 0.01
  of 255). So no claim that a better motion estimate buys a better generated
  frame is supportable here, and `TemporalGuides` is not on the critical path
  for frame generation the way it is for SR. `DlssgEvaluateSmoke` (gpu label)
  is the experiment, including the zero-motion control.
- `src/FrameRatePolicy.h` settles what a generated frame rate should be, which
  is a display question and not a source question. A multiple is accepted only
  when it divides the panel's refresh evenly: 30 fps doubles to 60 on a 60 Hz
  panel and every frame is scanned out once, while 24 fps on that panel is
  refused because 2x is 48 and 60/48 is 1.25 - generating there would trade one
  uneven cadence for another. The same 24 fps source on a 120 Hz panel takes 5x
  to exactly 120 and loses its 3:2 pulldown, which is the largest win available
  and the one a source-only rule ("under 45 fps, double it") cannot see. The
  seven refusals - unknown source rate, still image, variable frame rate,
  unknown refresh, source meets refresh, no even multiple, runtime refused -
  each name themselves, because "off" without a reason sends a viewer looking
  for a broken toggle.
  Nothing calls it yet, deliberately: choosing a multiplier needs the runtime's
  own `DLSSG.MultiFrameCountMax`, which means holding an NGX FrameGeneration
  feature, and standing a second undocumented NGX lifetime beside the RenoDX
  neural path is exactly what this project refuses to do casually. The cap is
  measured out of process by `DlssgProbeSmoke` until the render mode owns that
  lifetime. The rules are settled first because they decide whether that work
  is worth doing for a given source and panel at all.
- The Auto target no longer costs a display-mode query per toolbar paint.
  `EffectiveUpscaleHeight` is reached from `UpscalingAvailable` through
  `ToolbarState`, which every paint and every hover runs, and
  `EnumDisplaySettingsW` is not a function to call there. The mode is cached
  against the `HMONITOR` the window is on, re-read when the window lands on a
  different monitor, and dropped on `WM_DISPLAYCHANGE` and `WM_DPICHANGED` -
  the handle comparison cannot see a mode change under a window that has not
  moved, which is exactly what switching a 4K panel to 1080p does.
- Covered the persistence contract the Auto default depends on: absent
  `UpscaleAuto` with a legacy `UpscaleHeight=1440` loads as Auto, a rung pinned
  on this version survives a reload, returning to Auto keeps that rung
  underneath it, and an unrecognised height is not a selection.
- One claim did not survive checking and is recorded so nobody chases it. The
  UI regression suite was expected to pass once and then fail on a machine that
  had already run it, because `~PlayerApp` saves and the fullscreen block pins
  a manual rung. It did not: two consecutive runs both passed and the file kept
  `UpscaleAuto=1`. The reason is destructor order.
  `CheckFullscreenLifecycle` owns a second `PlayerApp` nested inside the outer
  one, so the inner destructor writes `UpscaleAuto=0` and the outer destructor
  overwrites it with 1 afterwards. So the suite was never broken - it was
  relying on nesting. Both ends are now pinned: the outer reset carries
  `m_upscaleAuto` with the rest of the persisted playback state, and the
  fullscreen block hands the rung back through `IDM_UPSCALE_AUTO` before its
  own destructor saves. Seeded with a hostile `UpscaleAuto=0`, the suite passes
  repeatedly and normalises the file.
- A display-mode change under an active Auto SR renderer is now reported rather
  than silent. The renderer is only swapped through `EnableUpscaling`, which
  recreates a device, a child window and an NGX feature, and a display change
  is the worst moment to touch a device - the same event can accompany a device
  loss. So the running output can outlive the rung Auto would now pick, and the
  log says which two sizes disagree and when it is rebuilt. Silence there would
  look like Auto ignoring the panel.
- `HexResultTextWide` is exported from `NeuralPreflight` so NGX result codes in
  wide diagnostics come from the one formatter the receipts already use.

## 0.23.0 - 2026-09-16

A verification pass over the whole tree after 0.22.0: seven audits in parallel
(session state machine, neural pipeline, decode and media, renderer, build and
CI, tests, security), every finding re-read in source, three of them refuted,
the rest fixed. Nothing here changes a cache key or retires an entry.

Three claims did not survive checking and are recorded so nobody chases them
again. Helpers are not orphaned by a force-kill: a live session with the worker
and six `ffmpeg` children two generations deep, all in kill-on-close job
objects, was reaped within four seconds of `Stop-Process -Force`; the earlier
"orphans" were a driver script's own leftovers. The shipped `ffmpeg` 9.0.1
verifies TLS certificates by default (self-signed, expired and wrong-host all
refused), so the "certificate verification off" finding was false for the pin,
though the flag is now passed explicitly because the pin will move. And the
"session gives up in 0.3 s on a seek into a hole" report described the 0.21.1
bug that 0.21.2 fixed: the lead is measured from the span around the playhead,
which is empty in a hole, so the attach path never fires there.

- Save converted video is only offered after an active session when a single
  render covered the whole video. It used to save the last rendered hole under
  the film's name: toggle neural on at 0:20 of a five-minute video, the session
  renders [0:20, end) then [0, 0:20), and the file written was twenty seconds
  long with no warning. `live_session::ExportableEntry` is the rule, and
  `Convert whole video` produces an entry it accepts.
- A cancel that reached the resident neural helper just after a job had
  finished no longer cancels the next job before its first frame. The reader
  raised one pending-stop flag for cancel, shutdown, a closed pipe and a dead
  parent alike, and a `Cancel` dequeued between jobs never cleared it; the
  window was the twenty milliseconds between the helper writing its result and
  the player noticing. Cancel is now its own flag, discarded when it arrives
  for a job that already reported; the terminal reasons still apply to the
  next job. Tested with a real command channel: job, result, late cancel, job -
  two served, none stopped.
- A neural render is refused, and its cache entry never served, when the
  backend reports fewer evaluations than frames were captured. The manifest's
  `nativeEvaluations` was a copy of the frame count, so the reuse gate that
  compares the two could not fail; it is now the backend's own tally, sampled
  after preroll, and the refusal that only applied to a reused evaluator
  applies to every job. The gate reads `>=` because a resubmitted frame
  legitimately evaluates twice.
- When the graphics device is removed or stops responding during playback, the
  player stops, says so in the status bar and rebuilds its renderer once
  instead of freezing the picture with the audio still running; if the rebuild
  fails it unloads with an explicit message. Proven with
  `ID3D12Device5::RemoveDevice()` while playing and while paused: rebuilt both
  times, playback resumed, no dialog. Inside the renderer, a `Present` that
  fails after its command list was submitted still publishes the frame slot,
  so the next frame cannot reset an allocator the GPU may be reading, and a
  device-loss HRESULT seen at `Present` or at any in-frame reset or close
  latches the same device-removed state the fence path does - an export no
  longer retries a frame up to 120 times on a dead device and reports it as a
  neural failure. The removed reason is logged once, and debug builds record
  DRED breadcrumbs. A second renderer that cannot drain within its budget ends
  the helper (exit 713), which the player treats as a crash and relaunches,
  instead of leaking a second device with its NGX lease.
- `VideoDecoder::Swap` carries the whole probed source. Six members - the
  hardware-decode memo key, the colour description, the raw colour tags, the
  pixel layout and the two sequential-open flags - were left behind, so after
  a YouTube quality reload the acceleration memo was keyed by the previous
  stream's codec and `ColorDescription()` answered for it. The probe state is
  one struct now, swapped as a unit.
- A frame-buffer allocation failure on the decode thread ends that playback
  with an error instead of terminating the player; the queue thread has the
  same guard the audio reader always had.
- Every `ffmpeg`/`ffprobe` child the media pipeline spawns has a wall-clock
  bound and is killed if it hangs; `ffprobe` output is capped at 1 MiB like
  every other capture; the probe timeout setting applies to local files too;
  a video whose file name starts with a dash probes and opens (the path goes
  after `-i` now).
- Resolved YouTube streams are fetched with `-tls_verify 1` and
  `-protocol_whitelist https,tls,tcp` in the downloader, the direct decoder and
  the audio player. Checked against the shipped `ffmpeg` with a resolved direct
  URL and an HLS manifest URL (both fetch), and against `file:` and `http:`
  inputs (both refused).
- The concat list a join writes lives in the temp directory, so a scanner
  holding it can no longer get it renamed into the published cache entry.
- Partial or invalidated payloads parked under `staging/` are reaped a few per
  launch, oldest first, never one whose player is still running. They used to
  accumulate until the whole cache was cleared by hand.
- Publishing a finished render hashes its payload once. `Promote` returns the
  verified entry; the post-rename check re-reads the manifest instead of
  re-hashing, and the player uses the returned entry instead of a third lookup.
  Reads of an ordinary cache hit still hash.
- Cancelling a neural settings preview, or opening another file during one,
  no longer leaves the buffering panel up and later previews refused. A neural
  toggle pressed during a seek no longer fires on the next file's first seek.
  A live render whose segment folder cannot be created no longer leaves the
  spinner on and every render action greyed out. An out-of-sync live render
  ends the session with a status-line reason instead of a dialog from inside
  the playback loop. Seeking a YouTube video during a session no longer
  re-asks whether to start a slow render, and declining is explained. A
  session that gives up because its coverage never reached the playhead hands
  the original back playing, not paused. A render that stops reporting
  progress says so when the player ends it. Closing the player stops a
  background download and update check. Rendered segments are removed when
  a file is closed, because playback is now closed before the folder is.
- The recent-videos file no longer accepts an entry whose path is a UNC share
  (a crafted entry would have made the next click connect out), and one bad
  record no longer discards the whole list. A YouTube video id must be exactly
  eleven characters everywhere.
- Both executables are built with Control Flow Guard and CET shadow-stack
  compatibility, and start by restricting DLL search to the application and
  system directories.
- Releases are created as drafts. The notes name
  `dlss5-video-player-v<version>-win64.zip`, which CI cannot build, so the
  release was public before that file existed; the maintainer now attaches it
  and publishes. The tag workflow can be re-run for the same tag from the
  Actions tab. Every action is pinned to a commit, the CI-built core zip
  carries a provenance attestation, and CI assembles and verifies that zip on
  every push and pull request - it is the uploaded artifact - instead of first
  exercising the packager at tag time. The packager ships the executable ctest
  ran rather than a clean rebuild. A pull request that bumps `VERSION` fails
  unless the changelog section and the README entry exist, through the same
  `tools/release_notes.ps1` the release body is built from. Packaged text
  files have a fixed line ending so the manifest hash does not depend on the
  packaging machine's git settings.
- Tests: every test has a time limit; the real-media suite reports itself
  skipped rather than failed without staged FFmpeg; the two GPU smokes are
  registered under a `gpu` label the portable run excludes; the resolver's
  refusal of reparse-point helpers runs on every machine using unprivileged
  reparse points instead of symlinks that needed Developer Mode; cached
  comparison playback is driven end to end on real media through the decoders
  it builds itself; the publish gate is fed a real joined render and a real
  truncated one; `PolicyTests` names the failing case, survives a crash in one
  and takes `--only=<name>`; `CHECK_EQ` prints both values; four tests that
  pinned log wording, JSON key order or exact labels assert behaviour instead;
  the unused `ReleasePackagePolicy.h` (a drifted copy of the packaging
  allowlist) is gone; configure fails if a `*_TESTING` seam reaches the player
  or the worker.
- The residue the audit batch left is closed. Every notice it added is a
  localized table entry (`neural.live.declined`, `neural.live.directory_failed`,
  `renderer.removed.*`, `renderer.stalled.*`, `recent.missing`). The live
  out-of-sync hand-back and the once-per-source pace confirmation have
  regression tests through the real handlers - the first by replacing the
  synchronized pair with a frame source whose numbering trips the resync guard,
  the second with a configurable answer to the captured message box. A seek
  whose GPU wait fails takes the same one-rebuild recovery as the tick instead
  of unloading silently. DRED is a runtime decision through the renderer's test
  hooks rather than a `_DEBUG` build switch, so the device-loss smoke now
  enables it, queries it and prints what it said; on this runtime an explicit
  `RemoveDevice()` yields no breadcrumbs because the runtime links them only
  for work still outstanding, and the renderer's log line says so in words
  ("DRED enabled; no breadcrumbs outstanding") rather than reading as
  "unavailable". The Debug configuration links again - NGX's debug-CRT import
  library is selected per configuration by one CMake variable - and the
  `RDX*.tmp` sweep has a permanent two-sided test. The NVIDIA SDK licence is
  normalised to CRLF (BOM stripped) as it is staged, so `PACKAGE_MANIFEST.txt`
  is byte-identical across clones with different `core.autocrlf`, and the
  package verifier rejects any packaged text whose line endings or BOM deviate.
  The three release-only workflow steps were exercised for real on a throwaway
  branch against this repository with the pinned actions: the provenance
  attestation was signed through Sigstore and is retrievable by the artifact's
  digest, `archive: false` uploaded the zip under its own name with the same
  digest, and the draft was created with both files and no tag materialised;
  the run, artifact, draft and branch were then deleted.
- Documentation: two agent-era planning documents (`docs/ECOSYSTEM_REVIEW.md`,
  self-dated to 0.17.1 with line-number citations into files rewritten since,
  and `docs/DLSS5_VIDEO_ROADMAP.md`, a status-and-plan note whose content the
  architecture and usage guides already own) are retired, along with an
  untracked hand-off note that had been hidden by a local exclude; every
  inbound link is rewritten and the one historical record that cited them says
  so. Stale claims in the shipped guides are corrected: the verification links
  that pointed at the first record only now point at the dated list, the
  update-check paragraph no longer says pre-releases are ignored (every
  published release is one, and the code counts them), the benchmark page no
  longer carries builder counts that the manifest contradicts, the packaged
  overview describes the portable suites plus the GPU label, and SECURITY.md
  states the process, TLS, CFG/CET, DLL-search and supply-chain properties the
  batch introduced. Every guide that describes the current tree now carries
  `_Verified against <version> (<sha>) on <date>._` under its title, and
  `tools/release_notes.ps1` - already the gate for a VERSION bump - fails when
  any of the seventeen is missing its line or stamped with an older version, so
  a release cannot ship a guide nobody re-read.

## 0.22.0 - 2026-09-16

Renders made before this release are retired, and that is deliberate. The cache
key now carries the driver version and a digest of the driver-store model
contents, so a render cannot survive a driver update or a model refresh, and
`NeuralWorker.exe` is hashed into the runtime digest, so a worker rebuilt with
different guide or cut logic retires its own entries. The manifest schema moved
4 → 5, which retires everything written before that through the schema gate.
Nothing here needs a cache wipe by hand: the first render of a video after
upgrading re-renders and republishes it.

One upgrade note: a `neural-runtime/NeuralWorker.exe` left over from a pre-v6
build is refused by the parent on the version check. That is the intended
fail-closed behaviour, but it looks like a broken helper - re-stage the runtime
from the release package rather than copying an exe over an old install.

- Seeking backwards on a YouTube stream no longer turns neural rendering off.
  A seek on a streamed source re-resolves the stream, and rendering is
  unavailable for that whole window - about two seconds. The session, meanwhile,
  correctly wanted to move its render to the hole the seek had landed in, tried
  to start a job there, and was refused twice inside 40 ms. Those refusals were
  counted as jobs that rendered nothing, which is what the two-strike give-up
  exists for, so the session stopped itself behind the user's seek and the
  toolbar read `Neural Rendering - Off`. A refusal that only means "not right
  now" no longer counts, and the session does not move its render while a
  resolution is in flight, because the commit at the end of it releases and
  restarts the session on its retained coverage anyway.
  Driven on a 1440p stream with no cached copy, a session attached at 1.3 s and
  a seek back to 0: the session retained its 6 rendered segments, restarted,
  rendered the `[0,2.2)` hole the seek landed in, attached there with 12.7 s
  buffered, published that entry and chained on to `[12.7,104.4)` - where before
  it gave up after two refusals and stopped. Only a local source was exercised
  by the earlier work on this: a cached copy makes seeks local, which is why the
  streamed path was the one still broken.
- A stream stops being a stream once its copy is on disk. A render always works
  from a complete local copy of the source, and a session acquires one - about a
  minute for a 60 MB 1440p trailer - but playback carried on reading the signed
  URL, so every seek out of rendered coverage was a re-resolution: a new URL, a
  decoder and renderer swap, the session released and restarted around it, and a
  resolution that can fail outright. Playback now moves onto that copy the first
  time a seek would otherwise have gone to the network, which is also the moment
  it is certain to exist. Seeks are local from there: **223-345 ms to first byte
  against 1.2-1.9 s** on the URL, with the session left alone.
  The job reports its local source and the cache key it lives under as soon as
  the acquisition lands, because the recent history - the player's only other
  route to them - is not written until a job completes, which on a first watch is
  exactly too late. Having the key early is what lets that render reuse the copy
  under one source identity. An earlier attempt started the copy as a plain local
  file instead; that keys its cache entries on a path, writes a local-file entry
  into the recent history for a file inside the cache, and leaves the copy
  unreferenced for the next eviction pass - the copy playback may by then be
  reading from.
  The key is remembered with the video it was reported for, so the initial-open
  commit - which deliberately does not go through the unload path - cannot hand
  a second video the first one's copy. Two 1440p trailers share a geometry, so
  the guard downstream would not have caught that.
- A finished render no longer holds the next hole back for 18 seconds. The
  session filled one hole, then sat with the GPU idle and a hole left in its
  range while playback crossed 15 seconds of video on the original - and the
  next job started 10 ms after the previous one's cache entry was published, so
  the publish was the wait. Timing its phases named it: joining 32 segments took
  131 ms and promoting the entry 253 ms, while **probing the joined file took
  27.8 s**. The probe asked FFprobe for `-count_frames`, which answers "how many
  frames are in here" by decoding every one of them - 1440p HEVC, 1833 frames.
  A video stream carries one packet per coded frame, so the same number can be
  had by demuxing: measured on a published entry, 1300 either way, 23.8 s
  against 0.05 s. The gate keeps its teeth - a 60%-truncated copy of that entry
  reads 889 packets, and one missing its tail segment 902 and 30.1 s against
  43.3 s, so both still fail the comparison that refuses a bad join.
  Re-driven on the same clip: publish **28.2 s -> 0.86 s** and **17.0 s ->
  0.73 s**, and the finished render to the next one's start **18.4 s -> 0.99 s**.
  The same validation runs on two other files: the acquired `source.mkv` a first
  watch downloads, and a materialized export. Both were paying a full software
  decode of their own length for a number the container can answer, so a first
  watch gets that time back too - the mechanism is shared, though the figures
  above are from the publish path.
  What could have made packets and frames disagree is pinned by a real-media
  test: two `libx264 -bf 2` parts, so coded and presentation order differ, are
  joined to an odd 31 frames and counted both ways; the video span is checked
  exactly, because Matroska stores no per-packet duration and the last one is
  derived - a tail that came back unknown would leave the span a frame short.
  Then one part is joined alone and required to read short, which is the
  refusal the gate exists for. Mutation-checked by dropping the tail duration:
  the span assertions fail, and restoring them passes.
  Each publish logs `concatMs`, `probeMs` and `promoteMs`, because the first
  measurement of this cost me a driven session to attribute.
  What is left of the publish is the join, and it is paid per segment file, so
  it grows with how long the render is. Measured on 1440p parts, timing only
  the two phases that remain:

  | case | parts | MiB | concat | probe |
  | --- | --- | --- | --- | --- |
  | 2 s parts, 1 min | 30 | 66 | 0.31 s | 0.06 s |
  | 2 s parts, 5 min | 150 | 331 | 1.33 s | 0.13 s |
  | 2 s parts, 15 min | 450 | 994 | 4.42 s | 0.36 s |
  | 30 s parts, 15 min | 30 | 1055 | 1.07 s | 0.28 s |

  The last row isolates the cause: the same content and the same bytes in 420
  fewer files joins 3.35 s faster, so the cost is about **9.8 ms per part**,
  not per megabyte. A 104 s clip is some 50 parts and half a second, which is
  why this is finished for now - but an hour-long render is ~1800 parts, about
  18 s of join plus 1.4 s of probe, and the idle gap between holes comes back.
  The fix for that is to stop gating the next hole on the publish at all:
  signal coverage-final at the receipt and let the join, probe and promote
  finish behind it, since they are FFmpeg processes and the helper is free
  again. That is a change to the single job slot, so it wants its own session
  and a long-content driven run to verify - not a tail-end edit to this one.
- The render no longer chases a playhead that is still moving. A viewer tapping
  the seek key moves it about once a second, and every move was a decision: the
  running job was cancelled and restarted on the new hole, and it produced
  nothing in between. Whichever hole they settle in is still there a moment
  later, so a decision waits for the playhead to stop for a second. The
  hand-back to the original is deliberately above that guard: a viewer who is
  seeking is never left in a buffering panel.
  Measured as an A/B on the same stream and the same driver, six back-seeks
  900 ms apart with the job mid-range, the only difference being the settle
  window:

  | six presses, 900 ms apart | without | with |
  | --- | --- | --- |
  | retargets | 5 | **1** |
  | job restarts | 5 | **1** |
  | segments rendered during the burst | **0** | **6** |
  | helper: launch / reuse | reuse | reuse |

  Each retarget landed 90-150 ms after its press, and the restarted job was
  cancelled before its first segment: five restarts, no output, with the first
  frames arriving 1.07 s after the last one. With the guard the running job is
  left alone and keeps publishing - six segments, about twelve seconds of video,
  while the viewer is still pressing - and the single retarget fires 1.0 s after
  the last press.
  The guard is not free for the viewer, and the first draft of this entry said
  it was sized "under" what it buys, which the same measurement contradicts: a
  restart reaches its first segment in about 1.1 s, so waiting a second defers
  the frames at the destination from 0.5 s after the last press to 2.5 s. The
  original plays there in the meantime. What is bought is render throughput -
  twelve seconds of video instead of none - not a faster picture where the
  viewer landed. The helper stays resident across all of it (`plan=reuse`), so
  a retarget costs the job restart, not a process launch: an earlier draft of
  this entry said three cold starts, which was inferred from an uncontrolled run
  and is wrong.
  At 400 ms between presses both builds retarget once: the seeks queue, and the
  existing "not while a seek is in flight" rule already coalesces them. That is
  also what a physically held key produces - Windows auto-repeats at about
  30 Hz after its half-second delay - so the window this guard owns is the
  tapped key or the clicked button, around a second apart, where each seek
  lands before the next arrives.
- The speed the status line reports is the speed of the render, not of the
  download. `0.48x real time` on a card measured at 16.3 ms/frame was the
  session's first minute of acquisition averaged into its render: the clock now
  starts at the first rendered segment, and the coverage it measures against is
  the coverage the session started with, so the first segment is not subtracted
  from every later reading. The same first watch now logs `pace=1.86x real
  time`, and the status line's chip - which only appears below 0.98x - is
  correctly absent for a card holding `FPS 31 rendered / 30 source`. Each job
  start logs the pace it measured, so the figure can be read from a log instead
  of a screenshot.
- A render is no longer started once playback is on the acquired copy but its
  key is unknown. The acquisition the job would fall back to needs the stream
  URL, and the loaded path is a local file by then, so it earned an instant "the
  source format or duration is unavailable" - and two of those in a row stop a
  session. That state now reports rendering as unavailable instead.
- A running render is no longer traded for a sliver. Moving a job costs about
  7.3 s of startup twice - once for the new hole and once to come back - so a
  hole narrower than that is left to the original, which covers it in the second
  or two it takes to cross. One driven session cancelled a job rendering
  `[38.6,104.4)` to chase a one-second hole. A paused viewer is the exception:
  that playhead is not moving, so the frame in front of it is worth rendering
  however narrow its hole. Nothing is stranded - the hole is still the first
  thing the session starts once the running job ends.
  The paused exception is pinned by test, not by a driven run.
  Seeking into an unrendered part of a streamed video, driven end to end with the
  seek target computed from the logged attach point rather than guessed: one
  refusal, playback moved onto the local copy, the render followed the viewer
  down through `[22.6,40.7)`, `[12.9,40.7)`, `[3.4,40.7)`, `[0,40.7)` keeping all
  6 rendered segments each time, and playback attached again **4 s later** at
  2.2 s. Zero YouTube re-resolutions after the seek (four before it - the open and
  three setup seeks, when no copy existed), zero session stops, zero give-ups,
  zero "format or duration unavailable" refusals, and the status line reads
  `49% rendered - FPS 30 rendered / 30 source` over two rendered regions.
- An active neural session renders the whole video and stays seekable everywhere.
  It used to render one forward run from the playhead, which left everything
  before it unrendered for good: seeking back landed on frames nobody had
  rendered, and the session "rebased" there - stopping the render, deleting every
  rendered segment the new playhead was not inside, and rendering the same
  seconds again. Coverage is a set of rendered regions now. A session's job is
  the whole source (or the marked range), filled hole by hole nearest the
  playhead first, and the status line says how much of it is done. Seeking into
  rendered frames plays them wherever they are; seeking into unrendered frames
  plays the original there at once and moves the render to that part of the video
  instead of discarding anything. The seek bar draws every rendered region, so
  the gaps are visible rather than implied by one long band.
  Measured on one driven session, a 113 s 1080p clip toggled on at 60 s and then
  seeked backwards four times: **4 retargets, each keeping all 18 segments
  rendered so far**, **2 attaches in the whole session** where an interim build
  oscillated 28 times, a seek landing 1.73 s inside a rendered segment paired
  with no fault, and two disjoint regions on the seek bar at 68% rendered. A
  later run of the same scenario finished the clip - `rendered all of [0,113) s
  over 61 segments` - and, played rather than paused with the render still
  filling ahead, held `FPS 30 rendered / 30 source · Dropped 0`. Nine defects
  are fixed here, three found by driving the player and six by review, among
  them: a job whose range was frame-snapped five ticks past its hole relaunched
  the helper on every tick; `ShouldAttach` ignored a seek in flight and attached
  at the position playback was leaving; a seek into the middle of a rendered
  segment was refused as a frame mismatch, because the frames a segment hands
  back after such a seek can be behind the playhead, and are now walked over; a
  file that ended inside its own declared window stalled playback at that
  boundary; the render pace was measured across the gap between two jobs; a
  YouTube quality reload would have adopted segments rendered at the previous
  resolution, which the pair refuses frame by frame, so the resolution is part of
  the retention key now; a session that gave up filling holes left the buffering
  panel up for good over the hole its dead target still named; and a job that had
  finished its hole was cancelled before its completion was processed, throwing
  away the cache entry it was about to publish for work already done.
  `NeuralSegmentIndex::Finished()` is gone: whether a session has more to do is a
  question about coverage, and one job ending answers only for its own hole. One
  limitation is recorded rather than fixed: each hole-filling job publishes a
  cache entry for its own sub-range, so a session that filled several holes does
  not leave one entry that makes the whole video a cache hit next time.
- Playback no longer collapses once a stream's source copy is in the cache. Asking
  whether one exists went through `LookupSource`, which authenticates the copy by
  hashing the whole payload - 60.5 MiB for a 1440p trailer, 63-86 ms - and the
  toolbar asks that on every paint, as do the status text and the live-session
  gates. Six answers a frame is half a second of hashing, so the UI thread ran two
  Ticks a second and the lateness test dropped nearly every decoded frame while the
  decoder kept handing over 29.7 fps. On the reported launch, the same clip and
  machine: **FPS 1 rendered / 30 source with 1877 dropped** before, **30 / 30 with
  25 dropped** after. The clock was never involved - playback position tracked the
  audio to five decimals throughout. The verdict is memoised against the payload's
  size and write time, for the miss as well as the hit, so only an acquisition that
  repairs or replaces the copy pays the hash again. The first session on a stream
  was always fast and every session after it was not, which is why this read as a
  playback regression rather than a cache query.
- A 1440p YouTube source plays at its own frame rate again. The decode pipe is
  sized to two frames so the child's decode overlaps our copy, but its ceiling was
  16 MiB: that is exactly two 1080p BGRA frames (15.82 MiB) and only 1.14 of a
  1440p one (28.12 MiB asked for). Since 0.21.0 Auto takes the tallest rung up to
  1440p, so the sources that lost the slack are now the common case. Measured on
  the Mafia trailer (AV1 2560x1440 30 fps, 3.68 Mbps), same 65-75 s window, same
  machine: **28.85 fps against a 30 fps source**, 98 partial pipe reads per frame
  and 5.23 ms of pipe read per frame. With the ceiling raised to cover two frames
  of the largest source the player accepts (2160p BGRA, 63.28 MiB): **29.93 fps,
  1.14 reads per frame, 1.41 ms** - identical to what the same trailer pinned to
  1080p already did (29.93 fps, 1.00 reads, 0.84 ms). Seeking was never the
  problem: teardown is under a millisecond and the ffmpeg respawn is 3 ms, while
  the 0.2-2.0 s a YouTube seek takes is the HTTP re-open.
- The **Motion vectors** guide switch changes the picture again. It only ever
  zeroed the CPU analysis grid, and from 0.20.0 the motion texture NGX reads is
  written by the hardware optical-flow resolve pass whenever the engine comes up -
  a pass that never saw the switch. So on every card measured here, turning motion
  vectors off moved the render cache key, paid a full re-render, and produced
  identical pixels. The control now travels with the guide grid and the flow pass
  is skipped when it is off, which is also what keeps the grid's zeros. Measured on
  an RTX 4080 SUPER over 12 frames of DLSS-SR at 2560x1440: before, `mv=1` and
  `mv=0` were byte-identical; after, they differ in 5.9 % of bytes, growing with
  accumulated history from 0 % on the first frame to 8.2 % by the twelfth, and
  `mv=1` is byte-identical to what it rendered before the fix.
- `GpuSourceConversion` is part of the render identity. It decides whether the
  model is shown NV12 converted on the GPU or BGRA delivered by ffmpeg - the input,
  not the encoding - and it was the one conversion switch outside the cache key, so
  a render made with it on could be served for a request with it off. The key's
  pipeline term now carries `nv12-source-v1` when the flag is on, appended rather
  than substituted, so every entry rendered on the default path keeps the key it was
  published under. This was the blocker on deciding that default.
- A neural-settings change made during playback says so. The picture keeps coming
  from the render the cache holds, and until now only the export path noticed the
  mismatch: Apply during cached playback saved the ini, could not preview a moving
  picture, and returned silently. The status line now carries "this is the previous
  render" until the settings match what produced it, or come back to it.
- `ctest` no longer erases `DLSSVideoPlayer.log`. `Log` opens
  `<module directory>/DLSSVideoPlayer.log` with `ios::trunc`, and
  `NeuralPrerenderTests` ran from the player's own directory while its render loop
  logged a stage table, so every test run wiped whatever the player had written.
  That suite now builds and runs under `build-upscaling/neural-prerender/`.
  `PolicyTests` still truncates the same file and is left alone on purpose: the
  build stages `ffmpeg`, `ffprobe`, `yt-dlp` and `deno` beside the player, and its
  YouTube bitrate test reads them from beside its own executable, so moving the
  target would make that test skip forever while `ctest` stayed green.
- The offline renderer's collaborators are chosen at runtime instead of by
  `OFFLINE_NEURAL_RENDERER_TESTING`, which is now gone along with the other four
  module macros. The macro did not add a test seam; it selected between two sets of
  adapters, and the production set - the real decoder, evaluator and encoder, about
  450 lines - sat behind its `#else` where no test target compiled it. Both sets now
  compile in every build, so a `D3D12Renderer`, `VideoDecoder` or `DLSSBackend`
  signature change breaks the test build rather than surviving to the release build,
  and residency is assertable at all for the first time. Injecting collaborators
  still does not execute the production adapters, and a partial injection is a
  `Protocol` failure rather than a silent fall back to a real decoder and encoder.
- `GpuSourceConversion` reads the source's colour description instead of assuming
  it. The decoder's existing ffprobe call also asks for `color_space`,
  `color_range`, `color_primaries` and `color_transfer`, and the GPU path is taken
  only for a matrix and range the conversion implements - BT.709 or BT.601, limited
  or full - with the shader compiled for exactly that pair. Anything else, a stream
  that declares nothing included, decodes to BGRA and ffmpeg converts it on the CPU:
  that costs pipe bandwidth and never colour, and never fails a render. Each render's
  log answers `GPU source conversion` with `accepted:` or `refused:` and the four
  tags it read. What it was worth: a JPEG is BT.601 full range by convention, and
  decoding one's NV12 with the previous hard-coded BT.709-limited coefficients lands
  mean 5.89 and max 33.0 eight-bit levels from its true colour, against 0.40 and 2.0
  for the program now selected. The BT.709-limited case is untouched - byte-identical
  shader bytecode, and three identical decoded-frame digests across before, a repeat
  of before, and after - so no cached render is re-coloured.
- The NGX log no longer cries wolf about its own core. NGX probes for the NGX core
  beside the executable first, a driver file no application ships, so every session
  logged two `failed to load NGXCore: 126` lines; the callback copied anything
  containing "error" and dropped the line four later that says the driver-store core
  loaded. That asymmetry was read here as "Super Resolution cannot start on this
  machine" and written into two documents before a real session disproved it. The
  expected probe is now labelled as routine and the load that decides the outcome is
  copied either way.
- `player_session.ps1` now keeps each session's *helper* log, not just the player's.
  `src/Log.h:28-31` names a log after the running module's directory, so
  `NeuralWorker.exe` writes its own `DLSSVideoPlayer.log` inside `neural-runtime/`,
  and `Log.h:33` truncates it on every helper start - which is where
  `idleVramPolicy=` and the post-job VRAM lines live. A three-session run therefore
  left only the last session's helper lines, so a policy A/B could assert one sample
  of three. Each session now records a `helperLogCopy` beside its JSON, and a helper
  log whose write time did not move is deliberately *not* copied: a session that
  started no helper leaves no file rather than inheriting the previous session's
  lines as its own sample.
- The benchmark corpus gained its first two long continuous-motion clips,
  `orig-film-motion-a` (272 frames, 11.3 s, dim interior with faces) and
  `orig-film-motion-b` (258 frames, 10.8 s, exterior tracking shot with the strongest
  sustained motion of any camera-original clip here). Every other camera-original
  clip is 1.3-5.5 s, which meant a still could never sit more than a fraction of a
  second past its shot's opening cut - too close to the guide generator's history
  reset for any judgement about a per-frame effect. Both are one continuous shot with
  their largest internal frame pairs inspected, and both are in
  `camera-original.digests.json`, so `corpus.py --check` now verifies nine clips.
- `blind.py` stopped two more ways of overstating a sample. It included every clip
  with runs on disk, so a ballot asked for two clips silently got a third from a
  previous round; `--clips` fixes that. And it drew each pair's frame independently
  over the whole pool, so a single-shot clip could hand the judge the same frame
  twice - one build drew 34, 34, 50 and 51 out of 66. Frames are now drawn without
  replacement with at least one excerpt length between them, the separation is
  recorded in `key.json` beside the guard, and a clip that cannot supply the pairs
  asked for says so instead of repeating itself. `--guard-seconds` makes the guard a
  per-ballot choice for the same reason.
- `blind.py` stopped two silent failures in the instrument that settles look
  questions. Building a ballot left the previous ballot's pairs in the directory the
  judge is told to look at, while overwriting the key that could score them - 12
  unscoreable images from a retired question sat beside the 8 live ones. And scoring
  an unfilled ballot printed a clean sweep of zeros and exited 0, which reads exactly
  like a measured tie; a ballot filled in against a since-rebuilt key read the same
  way. Stale pairs are now removed and counted out loud, and a ballot with nothing
  scored, or rows the key does not know, says so and exits non-zero.
- The render identity now covers what the pass actually evaluates. It carried no
  driver version, and `runtimeDigest` hashed the staged files while every run
  resolves its weights out of the driver store and `%ProgramData%\NVIDIA\NGX\models`
  - so a render produced on one driver was served *and* validated on a later one.
  Both terms are in the key now, with the driver version as a fallback that the
  preflight receipt records when a model root cannot be enumerated, rather than a
  silent one. The model-store content hash is deliberately uncached: the memo used
  elsewhere keys on path, size and write time, and Windows write times move in
  ~15 ms ticks, which is enough for a selector file rewritten in place at the same
  size to reuse a stale digest.
- A resident helper can now be asked to give its idle feature memory back:
  `[NeuralHelper] IdleVramPolicy=free` in `DLSSVideoPlayer.ini` returns **361 MiB
  of the 1061 MiB** an idle helper holds on this card, and costs **+0.604 s on
  every reuse** (2.400 s median against 3.004 s, +25.2 %, three sessions per arm,
  ranges not overlapping; a two-sample pair first read +0.70 s, so the direction
  held and the size came down). The default stays `keep`, because that reuse
  latency is the whole point of keeping a helper alive, and `free` remains one ini
  key away for a card where 361 MiB decides whether a second application fits. The
  post-job and idle samples are in `receipt.json` under `timing`, so the trade is
  checkable without a debugger.
- A helper that dies mid-job, or a device that is removed under it, now costs one
  restart instead of a lost render: the helper is relaunched once, re-preflighted
  and the job resumes. A second failure fails closed with the reason in the log,
  and neither path leaves an orphan holding VRAM.
- Hardware optical flow now runs in playback Super Resolution sessions. It used to
  require the decoded frame to already match the DLSS input size, so turning SR on
  silently dropped motion estimation to the CPU block matcher - a quality and
  performance cliff exactly where more quality was asked for. Sessions that were
  already using hardware flow are byte-unchanged.
- Five redundant full-target clears and a per-frame timestamp map/unmap are gone
  from the render path. Honest caveat: neither is measurable at 1080p or 4K on this
  card - they are removed because a clear that writes memory the next draw fully
  overwrites is waste, not because anything got faster.
- Not changed, and now measured to stay that way: the two `[Encoding]` GPU
  conversion defaults, which `docs/USAGE.md` has always described as off. Turning
  both on moves 2.7x fewer bytes across the decoder pipe and the capture readback
  and is worth +7 % throughput at 4K; it also costs 0.75 dB PSNR and doubles false
  motion on the same clip, and single-flag runs show neither half is free (capture
  alone -0.78 dB, decoder alone -0.64 dB) on an untagged test clip. On a
  `bt709`-tagged clip the decoder half is free (+0.067 dB) and only the capture
  half still costs (-0.53 to -0.64 dB): both conversion shaders hard-code BT.709
  while ffmpeg falls back to BT.601 for a stream that declares nothing, so most of
  that penalty was a colour-tag mismatch rather than lost detail. The shipped
  defaults stay until the source tags are read, which is the decoder side's blocker;
  the capture side has no quality cost left and is held only by the GPU-time note in
  USAGE.md, measured on a different card. The flags remain per-render either way.
- Every neural render was written **untagged and converted with BT.601**. The
  encoder stated colorimetry only when the GPU did the conversion, so a BT.709
  source became a file that declared no colour space while carrying 601 pixels -
  measured, not assumed: a pure-red frame through the shipped encoder line came back
  Y=81 U=90 V=240, which is the BT.601 prediction, at 1080p *and* 480p. Any player
  that assumes BT.709 for HD, which is the usual default, showed those colours
  shifted. Both paths now state `bt709`/`tv`, and the CPU path also converts with
  `out_color_matrix=bt709`, because tagging 601 pixels as 709 would have been worse
  than leaving them ambiguous. It also carries `setparams`, without which this
  FFmpeg drops the primaries and transfer tags in every container and encoder tried;
  both paths carry it, since it is metadata-only and pixel-exact through a lossless
  round trip. Verified on the same clip before and after: tags go from none to all
  four, and on the CPU path the pixels move with them (mean Y 57.12 -> 58.41)
  because that path's matrix changed too. The render identity's pipeline term moved
  to `bt709-export-v1`, since encoder arguments are not part of the cache key and
  renders made before this would otherwise have stayed valid hits.
- Describing the GPU-converted path's frames properly also removed a quality gap
  nobody had explained: with all four properties stamped, a capture converted on the
  GPU now matches the CPU path exactly (30.10 dB either way, where the GPU path had
  been 0.64 dB behind). The encoder had been handed frames it could not interpret,
  and the file it wrote was read on assumptions that did not match the shader that
  made it.

- A live session whose render key was already published never presented. The job
  was answered by the cache in about 50 ms, appended nothing to the segment index
  playback is bound to, and left it empty *and* finished - the one state where
  every decision said "wait": the attach saw zero lead, the rebase saw a playhead
  inside the range, and the player sat behind the buffering panel indefinitely.
  Coverage, not the job's verdict, now decides what a finished session plays, so a
  cache-hit session hands playback to the published entry at its coverage start
  and a session with nothing to show ends and returns the original. Reproduced
  deliberately on a 1 fps clip, where the snapped playhead and so the render key
  repeat by construction, and confirmed fixed on the same instrument.
  `live_session::PlanForCompletedSession` decides it, so it is tested without a
  window. This is the third form of the same family after 0.21.1 and 0.21.2.
- One session measured under load no longer decides what the machine can do. The
  render-pace profile kept a single sample per geometry and the newest replaced
  the oldest unconditionally, so a session that contended for the GPU wrote
  42.3 ms/frame - 3.7x this machine's idle mean - into `DLSSVideoPlayer.ini`, and
  the next sessions warned that the card could not keep up with a clip it renders
  faster than realtime. The profile now keeps the last five samples per geometry
  and forecasts from their median. Not the minimum: contention only ever inflates
  a measurement, and a forecast that exists to refuse sessions that cannot keep up
  must not erase slow evidence. The old single-value `Samples=WxH:ms` form still
  loads, as a one-sample ring.
- The `Neural cold start:` log line now carries the helper's five phases instead
  of five dashes. They arrive over the pipe while the render runs and the job only
  returns seconds after the attach, so the line - written at first picture - could
  never have held them; the receipt for the same render always did. A session that
  never started a helper now says `helper=none(cache-hit)` rather than printing
  dashes that read as a broken instrument.
- A neural render whose weak-arm scene cut fell within 0.6 s of the previous one
  was discarded, so the pass kept its accumulated history across a genuine shot
  change. Found on real footage: a hard cut 17 frames after its predecessor fired
  the weak arm cleanly and was suppressed by construction, because 17 is under the
  18-frame window 0.6 s means at 30 fps. The window is now 0.3 s, which the
  labelled corpus brackets from both sides - a transient returns 4 frames after
  the cut that opened it, and the shortest genuine shot is 17 - and the shortened
  window recovers that cut while leaving every synthetic score unchanged.
- The render cache no longer serves a schema-4 render that has no receipt. The
  receipt was verified only when the manifest carried a digest for it, so a
  manifest that simply omitted the digest was served as a verified render out of a
  user-writable directory. No released build ever wrote such an entry - schema 4
  and the receipt digest landed in the same commit - so nothing legitimate is
  invalidated. Legacy schema-3 entries and source entries keep their exemptions.
- The neural helper can now serve many jobs from one process. Protocol v6 adds a
  parent-to-helper command channel (`Hello`, `Job`, `Cancel`, `Shutdown`, and an
  outbound `Ready`), and a job is handed over as the argument vector the helper
  already validated, so nothing about what a job *is* changed. The helper is
  reused only while the runtime directory, runtime digest and neural-settings
  digest all match, holds the runtime lease only while a job runs, and exits
  itself after 30 s idle so the ~1 GiB of feature memory DLSS will not release is
  bounded in time. Toggling neural rendering on a second time in one player
  session now reaches a picture in **2.44-2.52 s** instead of 5.23-5.41 s, over
  four driven sessions: the reused job pays none of the process bring-up -
  `helperStart`, `runtimeReady`, `neuralInit`, `featureArm`, 2.19 s on the machine
  measured - and what is left is the first segment's encode and the attach, which
  residency cannot remove. A toggle that lands inside a range the previous session
  already rendered is still answered from the cache, in about 0.8 s, with no
  helper job at all. Measured on one Ada card at driver 610.47;
  `docs/VERIFICATION-matrix.md` carries the phases and the method.

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
