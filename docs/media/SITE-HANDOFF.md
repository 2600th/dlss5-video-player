# Site hand-off: media from 24 September 2026

For whoever maintains `site/`. Every asset below is already in the repository;
nothing here has been copied into `site/` or published anywhere. Each one is the
player's own output (a source frame beside the same frame of its render, or a
window capture of the player), cropped and scaled at most, and each has its
provenance recorded where the table says.

The renders are all at the player's **default** neural settings, on an RTX 4080
SUPER, driver 610.47, runtime lock `310.8.SF-v2`, player at `dced888` (0.25.0
plus the unreleased work in `main`); the window captures also have the playback
fix `f2ae230`. The site's current hero and gallery plates are from 22 September
at Intensity, Local tone and Local structure 2.0, so the look differs: the
default render is subtler.

## Changed in place: the site picks these up on its next build

| Path | Where the site uses it | What changed, and what the site needs to do |
| --- | --- | --- |
| `docs/media/neural-comparison-demo.mp4` | Demo (`build.ps1` copies it to `media/`) | New cut: 24.8 s, not 19.7 s, *007 First Light* and *GTA VI* at default settings plus the player's own compare views. Update the sitemap entry (description names The Matrix, duration 20 → 25), regenerate `site/src/assets/demo/demo-poster.jpg` (its frame 110 is now mid-sweep on the 007 face; frame 45 is the chosen poster, or take `docs/media/neural-comparison-poster.jpg` as is), and rewrite `site/src/assets/demo/provenance.json` (it credits Warner Bros. and describes the Matrix frame) |
| `docs/screenshots/current/neural-strength.jpg` | Copied to `media/`; shown in `site/src/index.html` and listed in its structured data | Now 1902x1023 at 150%: Image adjustments with the DLSS 5 mix slider over the 007 frame, not the Mafia trailer at 175%. The `<img>` in `index.html` still says `width="1493" height="1100"`, which will distort it: change them to 1902 and 1023, and check its caption and provenance line |

`docs/screenshots/current/neural-playback.jpg`, `original-comparison.jpg`,
`recent-videos.jpg`, `gta6-lucia-*.jpg` and `matrix-*.jpg` are unchanged and
still exist for the site; they were not deleted.

## New: offered for the site

| Asset | Size | Intended place | Alt text | Provenance |
| --- | --- | --- | --- | --- |
| `docs/media/neural-comparison-preview.webp` | 800x450, 8 fps loop, 2.4 MB | Demo fallback, or a hero loop where video autoplay is unwanted | A 25-second loop: a 007 First Light face split between the source and the player's DLSS 5 render, GTA VI playing with the divider, then the player's Difference, Side by side and loupe views and a render filling the timeline | [docs/media/README.md](README.md) |
| `docs/media/neural-comparison-poster.jpg` | 1920x1080, 116 KB | Demo poster | 007 First Light, one frame split down the face: the source on the left, the player's DLSS 5 render on the right | Frame 45 of the video; [docs/media/README.md](README.md) |
| `docs/media/stills/007-first-light-bond.png` | 1460x992 | **Hero** candidate, gallery | 007 First Light, one frame: the source on the left, the same frame from the player's render at default settings on the right, identical unscaled 700x880 crops | `stills/007-first-light-bond.provenance.json` |
| `docs/media/stills/resident-evil-requiem-flashlight.png` | 1460x992 | Gallery (night lighting) | Resident Evil Requiem, a face lit from below by a torch: the source on the left, the player's render on the right, identical unscaled crops | `stills/resident-evil-requiem-flashlight.provenance.json` |
| `docs/media/stills/gta6-lucia.png` | 1460x992 | Gallery (hair, golden hour); replaces the Intensity 2.0 Lucia plate if the site wants default settings throughout | GTA VI Trailer 2, Lucia at golden hour: the source on the left, the player's render at default settings on the right, identical unscaled crops | `stills/gta6-lucia.provenance.json` |
| `docs/media/stills/007-first-light-suit.png` | 1460x992 | Gallery (grey hair, wool suit) | 007 First Light, an older man in a dark suit: the source on the left, the player's render on the right, identical unscaled crops | `stills/007-first-light-suit.provenance.json` |
| `docs/media/stills/ac-shadows-low-key-limit.png` | 1460x992 | Gallery or limits section, **not the hero**: where the model does not help | Assassin's Creed Shadows, a face in deep shadow under a helmet: the source on the left; on the right the player's render crushes the face towards black and adds speckle | `stills/ac-shadows-low-key-limit.provenance.json` |
| `docs/screenshots/current/compare-wipe.jpg` | 1902x1023 | Gallery or features: the compare bar | The player in Wipe mode: the original left of a divider down a face, the DLSS 5 render right of it, with the compare bar below | [docs/screenshots/README.md](../screenshots/README.md) |
| `docs/screenshots/current/compare-difference.jpg` | 1902x1023 | Features: Difference | The player's Difference view: where the model changed the picture, amplified 4x, as brightness | same |
| `docs/screenshots/current/compare-2x2-toast.jpg` | 1902x1023 | Features: 2 x 2 and saving | The player's 2 x 2 view (original, DLSS 5, Difference, DLSS 5 at Mix 50%) with the toast confirming a saved comparison image | same |
| `docs/screenshots/current/saved-comparison-2x2.png` | 1266x790 | Features: "evidence you can share" | The PNG the player saved for its 2 x 2 view, with a footer recording the version, frame, view, settings digest and runtime | same; the footer is the provenance |
| `docs/screenshots/current/subtitles.jpg` | 1902x1023 | Features: subtitles | A subtitle drawn over the DLSS 5 frame; the text is a test file that says what it is | same |
| `docs/screenshots/current/neural-settings.jpg` | 1324x674 | Features: tuning | The Neural settings dialog at its defaults, grouped under Look, Quality and render time, Guides sent to the model, and Across frames | same |
| `docs/screenshots/current/player-start.jpg` | 1902x1023 | First run / download section | The start screen: the capability check and the seven game trailers with their YouTube thumbnails | same |
| `docs/media/social/card-1200x630.jpg` | 1200x630, 90 KB | **Social card** (`og:image`, `twitter:image`) | DLSS 5 Video Player: a 007 First Light face split between the source and the player's DLSS 5 render | [docs/media/README.md#social](README.md#social) |
| `docs/media/social/square-1080.mp4` | 1080x1080, 13 s, 0.9 MB | Social posts that take square video; not needed on the site | A 13-second square clip: the split face, the Difference view, a saved 2 x 2 comparison, and the project name | same |

## Notes for the site

- **Provenance line.** DESIGN.md's Real Pixels Rule asks for a mono provenance
  line beside every evidence raster. For the stills it can read, for example:
  `007 First Light · frame 1122 · 2560x1440 VP9 source · unscaled 700x880 crops ·
  default settings · RTX 4080 SUPER`. The `.provenance.json` beside each still
  has every field.
- **The honest miss belongs on the page.** `ac-shadows-low-key-limit.png` is
  there so the good frames are not the only evidence. The four good stills are
  the strongest frames found, not typical ones.
- **Credits.** 007 First Light © IO Interactive; Resident Evil Requiem ©
  Capcom; Grand Theft Auto VI © Rockstar Games; Assassin's Creed Shadows ©
  Ubisoft. The start screen shows YouTube's thumbnails for the seven trailers,
  which belong to their publishers.
- **Sizes.** The stills are 0.6-0.8 MB PNGs; the site's own pipeline
  (`make-hero-crops.ps1` and the gallery's AVIF/WebP ladders) should derive its
  web sizes from them rather than serve the PNGs.
- **Not included.** No new hero crop at 1920x1080 was made: the site's hero
  tool cuts its own from whole frames, and the whole 2560x1440 source and render
  frames for every still can be regenerated from the records (the video's
  `public/bond-1122-*.png` are exactly that for the 007 frame).
