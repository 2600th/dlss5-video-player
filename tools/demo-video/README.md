# Rebuilding the demonstration video

_Verified against 0.26.1 (d9c7b51) on 2026-09-25._

This folder builds `docs/media/neural-comparison-demo.mp4` with
[Remotion](https://www.remotion.dev) 4.0.520. What the video shows and where its
footage came from is in [docs/media/README.md](../../docs/media/README.md).

## You need

- Node.js, Python with numpy and Pillow, and FFmpeg (the repo's
  `external/ffmpeg/bin` is used when present, or pass `--ffmpeg-bin`)
- Two source trailers from the player's list, at its YouTube **Auto** quality
  (2560x1440): [007 First Light - Story Trailer](https://www.youtube.com/watch?v=trvIyyFt_MM)
  and [GTA VI Trailer 2](https://www.youtube.com/watch?v=VQRLujxTm3c)
- The player's whole-video render of each, at default settings. Open the
  trailer from **File > Game trailers**, run **DLSS > Convert & export >
  Convert whole video to neural video** and let it finish, then copy its folder
  out of `cache/v1/renders/` (and the source out of `cache/v1/sources/`)
  **before you next start the player**. On startup the player deletes the least
  recently used renders whenever the drive has less than 20 GB free.
- A folder of player window captures, taken with
  `tools/verification/capture-window.ps1`'s method: `difference.png`,
  `side-by-side.png` and `loupe.png` (the 007 frame 1122 in those views), and
  `band-000.png` to `band-049.png`, captures 200 ms apart of a live render
  filling the timeline. [docs/screenshots/README.md](../../docs/screenshots/README.md)
  says how they were taken.

## Build it

```powershell
npm ci
python prepare-inputs.py --bond <007 source.mkv> <007 render folder> `
                         --gta6 <gta6 source.mkv> <gta6 render folder> `
                         --captures <capture folder>
npm run preview   # open Remotion Studio
npm run render    # write docs/media/neural-comparison-demo.mp4
npm run poster    # write docs/media/neural-comparison-poster.jpg (frame 45)
```

Then rebuild the README loop:

```powershell
ffmpeg -i ../../docs/media/neural-comparison-demo.mp4 -vf "fps=8,scale=800:-2:flags=lanczos" `
  -c:v libwebp -quality 50 -compression_level 6 -loop 0 -an ../../docs/media/neural-comparison-preview.webp
```

`npm run card` and `npm run square` write the social card and square clip in
`docs/media/social/` from the same inputs; see
[docs/media/README.md](../../docs/media/README.md#social).

## How it fits together

`prepare-inputs.py` fills `public/`:

- `bond-1122-*.png`: the whole 2560x1440 source frame and the same frame of the
  render, lossless
- `lucia-*.mp4` (GTA VI frames 1896–1979): 1920x1080 native-pixel crops, one
  H.264 encode each at the source's own frame rate
- the player captures, and the PNG the player saved for the 2 x 2 view
  (`saved-2x2.png`), copied as they are
- the site's two fonts, so the video and the website match

It refuses a render that isn't complete and fully verified. It also checks by
picture that each pair lines up, and stops if it doesn't. Running it again on
the same inputs gives byte-identical files.

The composition lives in `src/`:

- `index.tsx`: the timeline, seven scenes over 744 frames at 30 fps
- `scenes.tsx`: one component per kind of scene - the sweep, the playing
  split, the player window, the render band, the end card
- `Split.tsx`: the divider that shows source and render side by side
- `social.tsx`: the 1200x630 card and the 1080x1080 clip, framed from the
  same inputs
- `brand.tsx`: the site's colours and the caption style

Every clip scene lasts exactly as long as its clip, so nothing is looped, held
or sped up, except the render band, which shows 10 s of captures in 5 s and
says 2x on screen. The player captures are cropped (the status row is left
out), scaled and pushed in, never retouched. The output is H.264, `yuv420p`,
CRF 19 and silent (set in `remotion.config.ts`), with no network access at
render time.

The code follows the official [Remotion agent skills](https://www.remotion.dev/docs/ai/skills),
version 4.0.527. It uses `<Video>` from `@remotion/media`, fonts loaded through
`@remotion/fonts`, `Interactive.Div` for text, inline `interpolate()` calls,
and one file per scene.

## Checking the result

Make sure all 744 frames decode, the file is 1920x1080 at 30 fps with no audio,
and the cuts at 4.0, 6.8, 9.8, 12.6, 16.0 and 21.0 seconds are clean. A clean
render doesn't prove the picture got better; that's what the side-by-side is
for.

`make-face-comparison.py` makes the still side-by-side figures in
`docs/media/stills/`, each with a provenance record; the commands are in
[docs/media/README.md](../../docs/media/README.md#comparison-stills).
