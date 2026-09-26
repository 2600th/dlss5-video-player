# Project website

The landing page at <https://2600th.github.io/dlss5-video-player/>, built from
this directory and deployed by `.github/workflows/pages.yml`.

Nothing here is reachable from the C++ build, and nothing in the C++ build is
reachable from here. The only coupling is read-only: the build reads `VERSION`
and copies screenshots out of `docs/`.

## Preview it

```powershell
.\site\build.ps1          # writes site/dist
.\site\test.ps1           # every case runs offline, from fixtures
```

Then serve `site/dist` over HTTP - opening `index.html` from the filesystem
works, but `release.json` will not load over `file://`:

```powershell
python -m http.server 8731 --directory site\dist
```

The scripts target **Windows PowerShell 5.1**, which ships with Windows, so no
install is needed. They are kept pure ASCII on purpose: Windows PowerShell reads
a script with no byte-order mark using the ANSI codepage, where the third byte
of a UTF-8 em dash is a smart closing quote, and PowerShell honours that as a
string delimiter. The same applies to the page's own text, which is why all file
I/O goes through `Read-TextFile` / `Write-TextFile` in `site/lib/Site.ps1`.

## Layout

```
site/
  build.ps1              resolve release -> substitute -> dist/
  test.ps1               assertions; CI runs these before deploying
  lib/Site.ps1           pure, offline helpers shared by both
  tools/                 one-off asset generation
  src/                   the page itself; index.html carries {{TOKENS}}
  fixtures/              release payloads the tests build against
  dist/                  build output, gitignored
```

## How the download stays current

`GET /releases/latest` **returns 404 for this repository.** Every release is
marked as a prerelease and that endpoint excludes prereleases; the equivalent
web URL quietly redirects to the release list rather than a download. So the
build resolves by tag instead:

1. Read `VERSION`, form `dlss5-video-player-v<version>`, and fetch that tag's
   release. This is the same convention `release.yml` enforces at publish time.
2. If that tag is not published yet, take the newest non-draft release.
3. If neither resolves, link the release list - and in CI, `-RequireRelease`
   turns that state into a failed build rather than a published page with no
   download.

CI has no way to know when the 327 MB complete package is attached by hand, so
a release carrying only the core zip is a supported state: the page offers the
core package and links the release page for the other one.

The page also re-checks the API once per session, but only when its baked data
is more than 24 hours old, and keeps the baked values on any failure.

## The GitHub star count

`build.ps1` reads the repository's star count from `GET /repos/{owner}/{repo}`
while it builds, and bakes it into the masthead, the source row beside the
downloads and the footer, with the build date in its tooltip. The page never
asks for it: no client-side API call, nothing to rate-limit. When the API does
not answer, the links render without a count and the source row states the
licence instead. Builds from a release fixture make no request at all;
`-RepoFixture site/fixtures/repo.json` supplies a count for the tests.

## Analytics

Google Analytics 4 is injected at build time from the `GA_MEASUREMENT_ID`
repository variable, and only when it is set:

- **unset** - no tag, no third-party request. This is what forks and local
  previews get.
- **set and valid** (`G-XXXXXXXXXX`) - the tag is injected once.
- **set and malformed** - the build fails, rather than shipping a page whose
  analytics silently collect nothing.

Events: `download_click` (with package and version), `demo_play`,
`compare_drag`, `gallery_flip` and `gallery_loupe` (with the scene).

Because the id is baked into the HTML, setting the variable does not change a
deployed page. Set it and then rerun the workflow:

```powershell
gh variable set GA_MEASUREMENT_ID --body G-XXXXXXXXXX
gh workflow run pages.yml
```

## One-time repository setup

```powershell
# Pages, built by Actions rather than from a branch
gh api -X POST repos/2600th/dlss5-video-player/pages -f build_type=workflow

# The About field on the repository page
gh api -X PATCH repos/2600th/dlss5-video-player -f homepage=https://2600th.github.io/dlss5-video-player/
```

## Assets

**Nothing large is committed twice.** The full-size pictures, the
demonstration video, its poster and the link-preview card live in
`docs/media/` and `docs/screenshots/current/`, and `build.ps1` copies the ones
the page names into `media/` at build time (the list is `$mediaFiles`). What
`site/src/assets/` commits is small: the hero's two 1920x1080 plates, and the
AVIF and WebP variants the page offers first. A built `dist/` is about 16 MB,
13 MB of it `media/` - the 6 MB demonstration, the five comparison stills as
PNG (0.6-0.8 MB each) and eleven player screenshots - but a visitor only fetches
the variants and, on request, the video; the full-size files are the
`<img>` fallbacks and the "full size" links.

`src/assets/hero/` holds the two comparison plates, regenerated by
`tools/make-hero-crops.ps1` (needs ffmpeg) with provenance in
`provenance.json`: a native 1920x1080 window of *007 First Light*, frame 1122,
from the source and from the player's default-settings render. The whole
frames are the demonstration's inputs (`tools/demo-video/public/bond-1122-*.png`,
written by `prepare-inputs.py`, not committed); the script refuses them unless
each, cut at the recorded box, is the committed still
`docs/media/stills/007-first-light-bond.png` pixel for pixel. It crops with
identical parameters and no scaling, colour adjustment or sharpening, and
**fails if the encode has made the two plates more alike** than the lossless
frames, because an encode that smoothed the difference away would misstate the
product.

`tools/make-responsive-images.ps1` (needs ffmpeg with libaom-av1 and libwebp)
writes every variant with one area downscale and one encode:

- `hero/` and `gallery/`: each plate of a pair, identically, and it **refuses
  a pair the encode has made more alike than its sources**.
- `stills/`: the comparison stills, whose pair is the two halves of one file;
  the same guard, measured on the halves.
- `screens/`: player screenshots, single pictures with no pair to guard.

What each file cost is in `variants.json` beside it, and `test.ps1` re-checks
every guard, and every declared width and height against the file's own
header. Rerun the tool with `-Force` after the hero plates change, or delete
the variants it should rewrite.

The gallery's 1:1 view is drawn above the film grain, the one exception to the
grain covering everything: it exists to show a file's pixels exactly.

Fonts in `src/assets/fonts/` are self-hosted Archivo and JetBrains Mono, so the
page makes no request to Google Fonts.

Screenshots and footage are credited to IO Interactive, Capcom, Rockstar Games,
Ubisoft and the publisher of Mafia: The Old Country;
see `docs/screenshots/README.md` for full provenance.
