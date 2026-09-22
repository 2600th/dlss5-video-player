# Rebuilding the demonstration video

This folder builds `docs/media/neural-comparison-demo.mp4` with
[Remotion](https://www.remotion.dev) 4.0.520. What the video shows and where its
footage came from is in [docs/media/README.md](../../docs/media/README.md).

## You need

- Node.js, Python with numpy and Pillow, and FFmpeg (the repo's
  `external/ffmpeg/bin` is used when present)
- The two source trailers: [The Matrix | 4K Trailer](https://www.youtube.com/watch?v=nUEQNVV3Gfs)
  and [GTA VI Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c), at the
  player's YouTube **Auto** quality (2560x1440)
- The player's render of each. Open the file in the player, press `D` and let it
  finish, then copy its folder out of `cache/v1/renders/` **before you next start
  the player**. On startup the player deletes the least recently used renders
  whenever the drive has less than 20 GB free.

## Build it

```powershell
npm ci
python prepare-inputs.py --matrix <matrix.mp4> <matrix render folder> `
                         --gta6   <gta6.mp4>   <gta6 render folder>
npm run preview   # open Remotion Studio
npm run render    # write docs/media/neural-comparison-demo.mp4
npm run poster    # write docs/media/neural-comparison-poster.jpg (frame 110)
```

Then rebuild the README loop and the site poster:

```powershell
ffmpeg -i ../../docs/media/neural-comparison-demo.mp4 -vf "fps=10,scale=880:-2:flags=lanczos" `
  -c:v libwebp -quality 58 -compression_level 6 -loop 0 -an ../../docs/media/neural-comparison-preview.webp
../../site/tools/make-demo-poster.ps1 -Force
```

## How it fits together

`prepare-inputs.py` fills `public/` with matched pairs. Each pair is a source
frame and the same frame from the render, with an identical crop:

- `matrix-2116-*.png` and `gta6-1940-*.png`: whole 2560x1440 frames, lossless
- `lucia-*.mp4` (frames 1896–1979) and `florida-*.mp4` (420–509): 1920x1080
  native-pixel crops, one H.264 encode each at the source's own frame rate
- the site's two fonts, so the video and the website match

It refuses a render that isn't complete and fully verified. It also checks by
picture that each pair lines up, and stops if it doesn't. Running it again on
the same inputs gives byte-identical files.

The composition lives in `src/`:

- `index.tsx`: the timeline, six scenes over 591 frames at 30 fps
- `scenes.tsx`: one component per kind of scene
- `Split.tsx`: the divider that shows source and render side by side
- `brand.tsx`: the site's colours and the caption style

Every clip scene lasts exactly as long as its clip, so nothing is looped, held
or sped up. The output is H.264, `yuv420p`, CRF 19 and silent (set in
`remotion.config.ts`), with no network access at render time.

The code follows the official [Remotion agent skills](https://www.remotion.dev/docs/ai/skills),
version 4.0.527. It uses `<Video>` from `@remotion/media`, fonts loaded through
`@remotion/fonts`, `Interactive.Div` for text, inline `interpolate()` calls,
and one file per scene.

## Checking the result

Make sure all 591 frames decode, the file is 1920x1080 at 30 fps with no audio,
and the cuts at 4.5, 6.9, 9.7, 13.3 and 16.3 seconds are clean. A clean render
doesn't prove the picture got better; that's what the side-by-side is for.

`make-face-comparison.py` makes the still side-by-side figures in
`docs/screenshots/`. The exact commands are in
[screenshot provenance](../../docs/screenshots/README.md).
