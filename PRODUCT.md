# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Stack

Static HTML/CSS/JS, rendered by a PowerShell build script, deployed to GitHub
Pages. User-selected on 2026-09-18 over Astro and a Node-based build, to keep a
C++ repository free of an npm dependency tree.

## Users

Someone who owns an RTX card, already knows what DLSS does for games, and wants
to know whether the same thing can be pointed at video. [inferred from the
product's own requirements and the README's framing]

They arrive from a link, a forum post, or the repository itself, and the
decision in front of them is whether to download 308 MB of unsigned community
software and run it. That decision is made on evidence, not on adjectives: the
question is always "does it actually look better, and by how much". [inferred]

A second, smaller audience builds from source or integrates: `docs/BUILDING.md`,
the core package, and the provenance attestation exist for them. [confirmed by
the repository's contents]

## Product Purpose

Run a video, photo or GIF through NVIDIA's DLSS 5 neural renderer, then look at
the result next to the original on the same frame. Success is the visitor
seeing the difference for themselves and deciding on it.

## Positioning

Rendering happens while you watch, not as an export queue: press `D` and
playback continues on rendered frames seconds later, while the session keeps
filling in the rest of the video nearest the playhead first. The original and
the render stay in step, so they can be compared on one frame at one moment
without moving the playhead.

Neighboring products do offline upscaling with a progress bar. The live render,
the whole-video coverage model, and the frame-accurate comparison are the parts
that could not be truthfully copied.

## Operating Context

Windows only. Needs an RTX card and NVIDIA driver 610.47 or newer. The player
ships as a zip that must be unpacked into a new, empty folder with
`neural-runtime/` kept beside the executable. Sources are local files, pasted
public YouTube links, or six bundled game trailers.

Renders are cached and reused only when the source, the runtime and the neural
settings all still match.

## Capabilities and Constraints

Confirmed from `README.md` and `CHANGELOG.md` at 0.23.0:

- Live rendering with seek anywhere, rendered or not; nothing rendered is
  discarded on seek.
- Compare as split, wipe or blend, without moving the playhead.
- Export: PNG/JPEG for photos, GIF for animations, MP4/MKV for video; MKV keeps
  source audio, subtitles and chapters without re-encoding.
- Optional DLSS Super Resolution on top, following the display rung by default.
- Neural settings at `Ctrl+N`, re-rendering the paused frame on change.
- Two packages: a complete 308 MB zip with the pinned neural runtime, and a
  31 MB core zip without it.
- The neural runtime is a modified, unsigned community build. This is not
  incidental and must never be softened or omitted.
- Not an NVIDIA product. Community project.
- Verified on an RTX 4080 SUPER and an RTX 5090, both on 0.26.1.

## Brand Commitments

- The name is "DLSS 5 Video Player".
- `assets/DLSSVideoPlayer.ico` is the existing mark.
- MIT licensed; third-party notices in `THIRD_PARTY.md`.
- The project's written voice is plain, specific and measured: it states
  numbers, names what broke, and does not sell. Marketing copy that overstates
  would be off-voice against every other document in the repository. [inferred
  from README and CHANGELOG prose, which is unusually disciplined]
- The community-project and unsigned-runtime disclaimers are binding and appear
  wherever a download is offered.

## Evidence on Hand

Real, in the repository, usable without fabrication:

- `docs/screenshots/current/matrix-original.jpg` / `matrix-neural.jpg` and
  `original-comparison.jpg` / `neural-playback.jpg` - one paused frame each,
  only Neural Rendering switched between the two captures (v0.25.0).
  Provenance in `docs/screenshots/README.md`.
- `docs/media/stills/` - five unscaled matched crops of a source frame and the
  player's render of it at default settings (007 First Light, Resident Evil
  Requiem, GTA VI, and one Assassin's Creed Shadows frame where the render is
  worse), each with a `.provenance.json`; `docs/screenshots/current/`
  `gta6-lucia-original.jpg` / `-neural.jpg`, the full frames behind the site
  hero.
- `docs/screenshots/current/neural-strength.jpg`, `recent-videos.jpg`,
  `player-start.jpg` (the current UI: the image adjustments window, the File
  menu and the start screen).
- `docs/media/neural-comparison-demo.mp4` (19.7 s, 1080p) with poster and WebP
  preview; `site/src/assets/hero/social-card.jpg` (1200x630 link preview).
- Release telemetry, read live on 2026-09-18: 4,959 zip downloads across 17
  releases; 127 stars; 12 forks.
- Seven verification reports under `docs/` and measurement data under
  `docs/measurements/`.

Absent, and not to be invented: testimonials, named users, press coverage,
pricing, benchmark claims beyond the measured numbers already published in
`docs/ARCHITECTURE.md` ("Decisions and the measurements behind them") and the
verification documents.

The screenshots are frames of official trailers (The Matrix, GTA VI Trailer 2;
older UI shots show The Godfather and The Witcher IV). Provenance is documented; the usage matches what the
README already does.

## Product Principles

1. **Show, do not claim.** The product's whole argument is a visible difference
   between two frames. Anything that describes instead of showing is weaker.
2. **State the caveats plainly.** Unsigned runtime, community project, Windows
   and RTX only. Burying these would betray the repository's voice.
3. **Numbers over adjectives.** The project measures things and publishes the
   measurements; the site should inherit that habit.
4. **The download must always be real.** A version, a size, a checksum, and a
   link that resolves.

## Accessibility & Inclusion

No product-specific standard was previously established. The comparison
interaction must be operable by keyboard and not depend on a pointer drag, since
it is the primary evidence on the surface and the only way to reach the argument.
