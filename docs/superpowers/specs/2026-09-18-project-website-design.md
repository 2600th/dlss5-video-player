# Project website

_Design, 2026-09-18. Written against 0.23.0._

A single marketing page for DLSS 5 Video Player, served from GitHub Pages at
`https://2600th.github.io/dlss5-video-player/`, whose download button always
names the current published build. The repository's About field points at it.

## Why

The README does two jobs at once: it sells the project to someone who has never
heard of it, and it documents the project for someone already using it. The
selling half is the weaker one, because GitHub's Markdown cannot show the one
thing that makes the case - the original and the render on the same frame, under
the reader's own hand.

The repository already holds the material for that: a matched
`godfather-original.jpg` / `godfather-neural.jpg` pair captured from one frame
with only Neural Rendering switched between them, provenance recorded in
`docs/screenshots/README.md`. A draggable comparison of those two files is the
entire pitch, and it needs a real page to live on.

## Goals

- One page that makes the case in its first screen, without scrolling or reading.
- A download that always resolves to the newest published build, naming its
  version, size and SHA-256, and never 404s.
- Google Analytics 4, switched on by repository configuration rather than by a
  commit, and silent in forks and local previews.
- No new dependency in a repository that currently has no JavaScript toolchain.

## Non-goals

- Rendering `docs/` or `CHANGELOG.md` as site pages. They are good, they are
  already readable on GitHub, and a second copy would drift. The site links out.
- A custom domain. `github.io` is sufficient and free of renewal risk.
- A cookie-consent banner. See "Accepted risks".
- Any change to the C++ build, `CMakeLists.txt`, or the release workflow.

## Decisions

| Question | Decision |
| --- | --- |
| Stack | Hand-written HTML/CSS/JS, rendered by a PowerShell script. No npm. |
| Scope | One landing page. |
| Direction | Cinematic dark, proof-first. |
| Download link | Resolved at deploy, refreshed client-side as a backstop. |
| Analytics | GA4, injected from a repository variable at build time. |

### Why PowerShell and not Node

The repository ships eight `tools/*.ps1` scripts and no JavaScript. A build step
in PowerShell matches that, runs on the maintainer's Windows machine with nothing
installed, and runs on `ubuntu-latest` under `shell: pwsh`, which is present in
the GitHub runner image. Node would also work and is more certainly present on
runners; it was rejected because consistency with the existing tooling is worth
more here than that margin of certainty, and because the alternative that
actually tempted us - Astro - would have added a lockfile and a dependency tree
to serve one page.

## Constraints discovered

These were measured against the live repository on 2026-09-18, not assumed.

**`GET /releases/latest` returns 404.** All 17 releases are marked
`prerelease: true`; there are no full releases. That endpoint excludes
prereleases, so it has nothing to return, and will keep returning 404 for as long
as the release workflow sets `prerelease: true`.

**The web URL `/releases/latest` does not 404 - it redirects to `/releases`.**
This is worse than a visible failure, because it looks like it works while
landing the visitor on an index of 17 releases instead of a download.

Any implementation that reaches for either of those is wrong for this repository.
The resolution below is built on the tag instead.

**The full package is attached by hand.** `release.yml` builds and attaches only
`DLSSVideoPlayer-v*-core-win64.zip`; the 308 MB complete package is attached by
the maintainer to the draft before publishing. Between a tag landing and that
attachment, the release exists with only the core zip on it. The site must render
that state correctly rather than emit a link to a file that is not there.

**Asset names, confirmed against v0.23.0:**

| Asset | Size |
| --- | --- |
| `dlss5-video-player-v0.23.0-win64.zip` | 308.7 MB |
| `dlss5-video-player-v0.23.0-win64.zip.sha256` | 85 B |
| `DLSSVideoPlayer-v0.23.0-core-win64.zip` | 31.3 MB |
| `DLSSVideoPlayer-v0.23.0-core-win64.zip.sha256` | 88 B |

Note the two zips use different name stems - `dlss5-video-player-` and
`DLSSVideoPlayer-` - and differ in case. Matching must be case-insensitive and
must not assume a shared prefix.

**Repository state at time of writing:** `has_pages=false`, no Actions variables,
`homepage` set to the releases URL, default branch `main`, and the authenticated
maintainer has `admin`. 4,959 zip downloads across all releases; 127 stars.

## Layout

```
site/
  README.md              preview, configure, deploy
  build.ps1              resolve release -> substitute -> dist/
  test.ps1               assertions; runs in CI before deploy
  src/
    index.html           template, carries {{TOKENS}}
    styles.css
    main.js
    partials/analytics.html
    assets/              favicon, social image
  fixtures/
    release-full.json    both packages attached
    release-core-only.json  full package not yet attached
    release-none.json    tag not published
  dist/                  build output, gitignored
```

`site/` is a sibling of `src/`, `docs/` and `tools/`. Nothing in it is reachable
from the C++ build, and nothing in the C++ build is reachable from it. The only
coupling to the rest of the repository is read-only: `build.ps1` reads `VERSION`
and copies media out of `docs/`.

`docs/` was rejected as the Pages source because it is already the project's
documentation folder; mixing a site build into it would make both harder to read.

## Build

`site/build.ps1 [-OutputPath dist] [-ReleaseFixture path] [-SkipMedia]`

1. **Resolve the release** (below) into an object holding version, tag, release
   URL, publish date, and for each package: name, URL, byte size and SHA-256.
2. **Resolve analytics.** Read `GA_MEASUREMENT_ID` from the environment. Empty or
   unset yields an HTML comment. Set and matching `^G-[A-Z0-9]{6,}$` yields
   `partials/analytics.html` with the ID substituted. Set and *not* matching that
   pattern is a build failure - a typo must not silently produce a page that
   collects nothing.
3. **Substitute tokens** in `index.html`.
4. **Copy media** from `docs/screenshots/current/` and `docs/media/`, and static
   files from `src/`, into `dist/`.
5. **Write `dist/release.json`** - the same resolved data, for the client-side
   backstop to compare against.

`-ReleaseFixture` makes step 1 read a file instead of the network, which is what
lets `test.ps1` cover the failure states without one.

### Resolving the release

In order, stopping at the first that yields a published release:

1. Read `VERSION`, form `dlss5-video-player-v$version`, and
   `GET /repos/{owner}/{repo}/releases/tags/{tag}`. This reuses the exact tag
   convention that `release.yml` already enforces at publish time, so the site and
   the release gate cannot disagree about what the current version is.
2. `GET /repos/{owner}/{repo}/releases?per_page=30`, discard drafts, take the
   newest by `published_at`. Covers the window after a `VERSION` bump merges but
   before its tag is published.
3. Neither - render the page with the download section pointing at `/releases`,
   labelled as the release list rather than dressed up as a direct download.

`GITHUB_TOKEN` is sent when present, which is always in CI, giving 5,000 requests
an hour instead of 60.

Within a resolved release, packages are selected case-insensitively:

- full: `dlss5-video-player-v*-win64.zip`
- core: `*-core-win64.zip`

Each package's `.sha256` sidecar is fetched and its hash inlined, so the page can
show a copyable checksum. A missing full package is a supported state, not an
error: the page then offers the core package and links to the release page for
the complete one. A missing *core* package is also tolerated. Both missing
degrades to case 3.

### Client-side backstop

The deploy workflow triggers on `release: published`, so the baked data is
normally correct within a minute of publication. The client-side check exists only
for the case where that run failed. On load, `main.js` compares `release.json`'s
build timestamp against now; if it is more than 24 hours old, it makes one
unauthenticated request for the newest release and updates the download section if
the version differs. Any failure - rate limit, offline, schema change - is caught
and discarded, leaving the baked values in place. The result is cached in
`sessionStorage` so a visit costs at most one request.

## Page

Dark only, with `color-scheme: dark` declared so scrollbars and form controls
match rather than flashing white. The palette is near-black with a single accent;
the footage supplies the colour.

1. **Hero.** The Godfather pair under a drag handle, full-bleed, above the fold,
   with the headline and the download button over it. A slow automatic sweep on
   first view demonstrates the handle; it does not repeat.
2. **What it does.** Renders while you watch; seek anywhere, rendered or not;
   compare without moving the playhead; export. Four points, each with a
   supporting screenshot from `docs/screenshots/current/`.
3. **Demo.** The 22-second video behind its poster.
4. **Download.** Both packages with version, size and checksum; requirements
   (Windows, RTX card, driver 610.47+); the unsigned-runtime notice, stated
   plainly rather than buried.
5. **FAQ.** Four or five questions, including what the neural runtime is and why
   it is unsigned.
6. **Footer.** Links to README, usage, building, troubleshooting, third-party
   notices, and the repository.

The comparison slider is an `<input type="range">` given a custom appearance.
Keyboard operation, touch, and screen-reader semantics come from the element for
free; JavaScript only mirrors its value into a CSS custom property that drives the
top image's `clip-path`. If the script fails, the two images are still there and
still labelled.

## Accessibility and performance

- Every control reachable and operable by keyboard; visible focus throughout.
- `prefers-reduced-motion: reduce` suppresses the hero sweep and all transitions.
- Contrast at WCAG AA against the near-black background, verified rather than
  eyeballed.
- The two hero images are preloaded with explicit `width`/`height`; everything
  below the fold is `loading="lazy"`. No layout shift.
- The 5.3 MB demo video is `preload="none"` behind its poster and loads only on
  click. It is never autoplayed.
- Budget: under 700 KB transferred for the initial view. The page must be readable
  and the download usable with JavaScript disabled.

## Deploy

`.github/workflows/pages.yml`:

- **Triggers:** push to `main` touching `site/**`, `VERSION`,
  `docs/screenshots/current/**`, `docs/media/**`, or the workflow itself;
  `release: [published]`; `workflow_dispatch`.
- **Permissions:** `contents: read`, `pages: write`, `id-token: write`.
- **Concurrency:** group `pages`, `cancel-in-progress: false`. A deploy that is
  publishing should finish rather than be killed by a later push.
- **Build job** on `ubuntu-latest`: checkout, `pwsh site/build.ps1`,
  `pwsh site/test.ps1`, `actions/upload-pages-artifact`.
- **Deploy job:** `actions/deploy-pages`.

Every action pinned to a commit SHA with the version in a trailing comment,
matching `release.yml`. `actions/checkout` reuses the SHA already pinned there.
The tests run before the artifact uploads, so a broken page is never deployed.

## Testing

`site/test.ps1`, run in CI before deploy and runnable locally. Written before
`build.ps1`.

| Assertion | Guards against |
| --- | --- |
| `release-full.json` yields both packages with correct names, sizes, URLs | Ordinary path |
| `release-core-only.json` yields no full package, and the page still offers core plus a release link | The hand-attachment window |
| `release-none.json` falls back to the release list without emitting a download URL | A 404 button |
| Package matching is case-insensitive and tolerates the two differing name stems | The `DLSSVideoPlayer-` / `dlss5-video-player-` split |
| `GA_MEASUREMENT_ID` unset or empty leaves no `gtag` reference in `dist/` | Tracking in forks and local previews |
| A valid ID appears exactly once, with the ID substituted | Double-tagging |
| A malformed ID fails the build | Silent no-op analytics |
| No `{{` survives anywhere in `dist/` | Any token added to the template and not to the script |
| Every site-relative `href`/`src` resolves to a file in `dist/` | Broken links, missed media copies |
| `dist/index.html` states the version from `VERSION` | Stale bake |

## Repository configuration

Documented in `site/README.md` for the record, and performed with the
authenticated CLI after the first successful build:

```
gh api -X POST repos/2600th/dlss5-video-player/pages -f build_type=workflow
gh api -X PATCH repos/2600th/dlss5-video-player -f homepage=https://2600th.github.io/dlss5-video-player/
gh variable set GA_MEASUREMENT_ID --body G-XXXXXXXXXX
```

The About field is repointed only once the site answers, so it never points at a
404. The GA4 ID is supplied last; because it is baked at build time, setting the
variable must be followed by a `workflow_dispatch` run for the tag to appear.

## Accepted risks

**No cookie banner.** GA4 on a developer-tool landing page is ordinary practice
and IP handling sits with Google, but this is a judgment call, not a determination
of law, and it is the maintainer's to reverse. The analytics partial is a single
injected file, so replacing GA4 with a cookieless counter is a contained change.

**Copyrighted frames.** The hero pair and several screenshots are frames of The
Godfather, GTA VI and Witcher IV trailers. This is the same use the README already
makes, in a more prominent context. The provenance in `docs/screenshots/README.md`
documents it. The hero pair is referenced through one token, so substituting
different footage is a one-line change.

**Every release is a prerelease.** The site works regardless, because it resolves
by tag. But it means `/releases/latest` is permanently dead for anyone else
integrating with the project. Out of scope here; worth fixing upstream.

**`pwsh` on the runner image.** Present today, but image-dependent in a way Node
is not. If it is ever removed, the fix is a contained rewrite of two scripts.

## Open items

- GA4 Measurement ID, to be supplied by the maintainer at the end.
- Commit SHAs for `actions/upload-pages-artifact`, `actions/deploy-pages` and
  `actions/configure-pages` must be resolved at implementation time with
  `gh api repos/{owner}/{repo}/git/ref/tags/{tag}` rather than copied from
  documentation.
