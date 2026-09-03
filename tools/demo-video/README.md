# Rebuild the 30-second demonstration

Small Remotion 4.0.520 composition around genuine player recordings. The official
[Remotion agent skills](https://www.remotion.dev/docs/ai/skills) were evaluated
and applied: `remotion-create`, `remotion-markup`, `remotion-render`, and
`remotion-studio`, from skills revision
`54e9b19a612897171e0b3b242e01c2badba4a272`.

From this directory, with Node.js and FFmpeg available:

```powershell
npm ci
./prepare-inputs.ps1
npm run preview
npm run poster
npm run render
```

`prepare-inputs.ps1` copies the maintained hero and recovers the 9-second and
15-second genuine app recordings from the delivery MP4. This works from a clean
checkout, but adds one lossy encoding generation. For the original capture
quality, supply the session's ignored raw directory instead:

```powershell
./prepare-inputs.ps1 -RawCaptureDirectory ../../build/media-refresh/witcher
```

The raw inputs are `face-raw.mp4` (take at 9–18 seconds) and
`playback-on-raw.mp4` (take at 1–16 seconds). Both targeted 30 fps using
FFmpeg's desktop `gdigrab`, with a 1442 × 932 region at screen position 59,52.
The recordings contain capture timestamp gaps; Remotion samples them at 30 fps
while preserving elapsed time and playback speed.
These coordinates describe this session only. For a new recording, use the
computer-use skill to observe the current window and derive fresh bounds.
Capture the desktop region: client-only GDI capture can miss the D3D surface.
Keep the player foreground and helper windows out of the recorded region.
Wait for cache promotion to finish before opening its files in FFmpeg or other
inspection tools: an external reader can prevent Windows from moving staging
files. Inspect a separate copy when the player is still working.

Use the real toggle in the paused comparison, then leave Neural Rendering On
throughout moving playback and the closing card. Do not simulate UI or change
playback speed. Keep source resolution, neutral image adjustments and upscaling-off
settings. Check the selected faces for blinking and blur before recording.
This edit uses The Witcher IV's daylight village sequence. Inspect every shot
used in the export for nudity, sexual activity and sexualized imagery.

The composition is 900 frames at 30 fps. The two moving/paused recordings remain
at native capture size; titles and notices sit outside them. `src/index.tsx`
defines the edit. `remotion.config.ts` sets H.264, yuv420p and CRF 19. The output
is deliberately silent. There are no remote assets or runtime network calls
in the composition.

The optional `make-face-comparison.py ORIGINAL NEURAL` requires Python, Pillow
and FFmpeg. It extracts frame 3375 and produces identical 700 × 880 unscaled
crops for the documentation. The two arguments must refer to the synchronized
source and cache documented in [screenshot provenance](../../docs/screenshots/README.md).

After rendering, preview the whole file, inspect the cuts at 3, 12 and 27 seconds,
verify all 900 frames decode, and confirm 1920 × 1080 / 30 fps with no audio.
Do not interpret a successful export as proof of image-quality improvement.
