# Rebuild the 22.6-second demonstration

Small Remotion 4.0.520 composition around genuine player recordings. The official
[Remotion agent skills](https://www.remotion.dev/docs/ai/skills) were read and
applied for this edit - `remotion-best-practices`, `remotion-markup` and
`remotion-render`, at skills version `4.0.523` - which is why the composition
uses `<Video>` from `@remotion/media`, `Interactive.Div` for the text blocks,
`useVideoConfig()` for the frame rate and inline `interpolate()` calls instead of
precomputed CSS transforms.

From this directory, with Node.js and FFmpeg available:

```powershell
npm ci
./prepare-inputs.ps1 -RawCaptureDirectory ../../build/media-refresh-20260912/takes2
npm run preview
npm run poster
npm run render
```

`prepare-inputs.ps1` re-encodes the four recordings the composition loads -
`playback-godfather.mp4` (6.8 s), `compare-godfather.mp4` (6.8 s),
`compare-gta6.mp4` (6.7 s) and `playback.mp4` (4.9 s) - from the session's ignored
working directory into `public/`, keeping each take's own start, because `scenes`
enters every recording at its `trim` second and the measured `zoom` / `wipe`
instants are offsets into the take. `-RawCaptureDirectory` is required: the
delivered MP4 cannot stand in for the raw takes, because it holds no frame before
any scene's `trim` and its two comparison scenes already carry the ORIGINAL /
NEURAL RENDERED chips inside the window a crop would take.

The 12 September 2026 session delivered two longer takes, `godfather-toggle.mp4`
(11 s) and `gta6-play.mp4` (16 s); where each of the four assets above was cut out
of them is not recorded here, so those four files are what the operator has to
place in the capture directory. Both session takes were recorded with FFmpeg's
`gdigrab` at 30 fps over the player's **visible** window frame -
`DWMWA_EXTENDED_FRAME_BOUNDS`, 1442 x 932, not `GetWindowRect`, which includes an
invisible resize border and would drag a strip of the desktop into the shot.
`build/media-refresh-20260912/capture.py` is the throwaway driver that placed the
window, seeked by pressing the player's own timeline, pressed the real hotkeys
and started the recorder; it is not part of the product.

Both takes are live neural rendering as it ran on an RTX 5090: the first is the
real `Ctrl+Alt+D` toggle on a paused frame, the second is uninterrupted playback
with the neural view left on. Do not simulate UI, retime a take or change playback
speed. Keep source resolution, neutral image adjustments and upscaling off.
Check the selected faces for blinking and blur before recording, and inspect
every shot used in the export for nudity, sexual activity and sexualized imagery.

The composition is 678 frames at 30 fps - `Math.round(total * 30)` over a `total`
of 22.6 s, the last scene's `start` of 19.4 plus its `length` of 3.2. `scenes` in
`src/index.tsx` holds the five shots: the title over Godfather playback to 3.4 s,
the magnified Godfather wipe to 9 s, the same controls on a paused GTA VI frame to
14.6 s, uninterrupted GTA VI playback to 19.4 s, and the download card to 22.6 s.
No scene changes playback speed, so nothing is stretched. The recordings stay at
their captured 1442 x 932 size; titles and notices sit outside them.
`remotion.config.ts` sets H.264, `yuv420p` and CRF 19. The output is
deliberately silent. There are no remote assets or runtime network calls in the
composition.

The optional `make-face-comparison.py ORIGINAL NEURAL` requires Python, Pillow
and FFmpeg. It extracts one frame from each side and writes identical unscaled
700 x 880 crops for the documentation; `--neural-frame` is the index inside the
render, which starts at the render range's first frame rather than the source's.
See [screenshot provenance](../../docs/screenshots/README.md) for the exact
arguments used for the two current figures.

After rendering, preview the whole file, inspect the cuts at 3.4, 9, 14.6 and 19.4
seconds, verify all 678 frames decode, and confirm 1920 x 1080 / 30 fps with no
audio. Do not interpret a successful export as proof of image-quality improvement.
