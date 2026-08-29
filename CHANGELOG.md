# Changelog

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
