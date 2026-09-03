# Changelog

## Unreleased

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
