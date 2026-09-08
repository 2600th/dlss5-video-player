# Changelog

## Unreleased

- Open media without rendering it first. A YouTube URL plays from its stream as
  soon as it resolves (about 6 seconds here), a local file replays a validated
  cache entry when one exists, and otherwise the original plays immediately;
  In/Out markers, the frame and clip previews, **Render marked range** and
  **Render whole video** decide what is rendered.
- Acquire a streamed source when its first render is requested, not when the
  video is opened, and re-resolve the page URL once when the resolved stream
  URLs have expired. A source played from its cached copy seeks locally instead
  of re-opening the network stream, and end of file ends playback.
- Show what is rendered on the timeline: the marked range runs along the top in
  green, played progress in the middle, and a violet stripe along the bottom
  marks the part of the source that has cached neural frames. The status line
  leads with the In/Out timecodes, or with how to mark a range.
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
