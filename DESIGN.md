---
name: DLSS 5 Video Player — Project Site
description: An image-quality breakdown in true black: one ink, hairline rules, and every number measured.
colors:
  ground: "#050506"
  text: "#ecebe8"
  text-dim: "#a4a099"
  text-meta: "#837e77"
  flag: "#ff6a1a"
  flag-lift: "#ff8340"
  flag-wash: "rgba(255, 106, 26, 0.09)"
  ink-on-flag: "#120602"
  line: "rgba(255, 255, 255, 0.15)"
  line-soft: "rgba(255, 255, 255, 0.075)"
typography:
  display:
    fontFamily: "Archivo, system-ui, -apple-system, 'Segoe UI', sans-serif"
    fontSize: "clamp(2.7rem, 6.2vw, 5.1rem)"
    fontWeight: 800
    lineHeight: 0.96
    letterSpacing: "-0.038em"
    fontVariation: "'wdth' 92"
  headline:
    fontFamily: "Archivo, system-ui, -apple-system, 'Segoe UI', sans-serif"
    fontSize: "clamp(1.65rem, 3.1vw, 2.6rem)"
    fontWeight: 750
    lineHeight: 1.08
    letterSpacing: "-0.028em"
    fontVariation: "'wdth' 94"
  title:
    fontFamily: "Archivo, system-ui, -apple-system, 'Segoe UI', sans-serif"
    fontSize: "1.3rem"
    fontWeight: 700
    lineHeight: 1.3
    letterSpacing: "-0.02em"
  body:
    fontFamily: "Archivo, system-ui, -apple-system, 'Segoe UI', sans-serif"
    fontSize: "1.0625rem"
    fontWeight: 400
    lineHeight: 1.62
    fontVariation: "'wdth' 100"
  label:
    fontFamily: "Archivo, system-ui, -apple-system, 'Segoe UI', sans-serif"
    fontSize: "0.7rem"
    fontWeight: 650
    letterSpacing: "0.15em"
    fontVariation: "'wdth' 78"
  data:
    fontFamily: "'JetBrains Mono', ui-monospace, 'Cascadia Mono', Consolas, monospace"
    fontSize: "0.76rem"
    fontWeight: 400
    letterSpacing: "-0.01em"
    fontFeature: "tabular-nums"
rounded:
  none: "0"
spacing:
  gutter: "clamp(1.25rem, 4vw, 2.75rem)"
  section-block: "clamp(3.5rem, 8vw, 7rem)"
  beat-block: "clamp(2.25rem, 5vw, 4rem)"
  row: "0.75rem"
  measure: "64ch"
components:
  button-primary:
    backgroundColor: "{colors.flag}"
    textColor: "{colors.ink-on-flag}"
    rounded: "{rounded.none}"
    padding: "0.85rem 1.5rem"
    typography: "{typography.title}"
  button-primary-hover:
    backgroundColor: "{colors.flag-lift}"
    textColor: "{colors.ink-on-flag}"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.text}"
    rounded: "{rounded.none}"
    padding: "0.85rem 1.5rem"
  button-ghost-hover:
    textColor: "{colors.flag}"
  package-cta:
    backgroundColor: "transparent"
    textColor: "{colors.text}"
    rounded: "{rounded.none}"
    padding: "0.7rem 1.25rem"
  package-cta-hover:
    textColor: "{colors.flag}"
  checksum-button:
    backgroundColor: "transparent"
    textColor: "{colors.text-meta}"
    rounded: "{rounded.none}"
    padding: "0.2rem 0"
    typography: "{typography.data}"
  checksum-button-hover:
    textColor: "{colors.flag}"
  readout-key:
    textColor: "{colors.flag}"
    typography: "{typography.label}"
  readout-value:
    textColor: "{colors.text-dim}"
    typography: "{typography.data}"
  notice:
    backgroundColor: "{colors.flag-wash}"
    textColor: "{colors.text-dim}"
    rounded: "{rounded.none}"
    padding: "clamp(1.1rem, 2.5vw, 1.6rem)"
---

# Design System: DLSS 5 Video Player — Project Site

## Overview

**Creative North Star: "The Cutting Bench"**

This is a darkroom bench with a film strip on it, not a product page. The ground is true black, dusted with a fixed film grain borrowed from the subject's own world, and the only thing that carries colour is a single flag orange used the way a cutting bench uses a splice mark: to point at exactly one thing. There is no glow, no gradient card, no accent-tinted panel. When the page wants to say "here", it draws a two-pixel seam, a six-pixel tick, or a one-pixel rule — a mark, never a hue wash.

Density is the argument. The page is willing to pack real provenance — resolution, timestamp, frame counts, driver version, capture settings — against otherwise bare space, because the product's claim is measurable and the page's job is to let a visitor check it. That density is set in mono and it is always a measured value; prose never wears monospace as costume. Everything else is a condensed grotesk: heavy, tightly tracked headlines at negative letter-spacing, and small uppercase labels drawn at the width axis' condensed end.

Structurally the page refuses containers. Sections are separated by hairline rules and by the whitespace of a generous section rhythm, not by boxes with corners. Nothing has a radius; nothing floats; nothing is elevated. The single largest element on the page is a real photograph at real resolution, and the interactions that matter operate directly on it.

**Key Characteristics:**
- True black ground (#050506) under a fixed feTurbulence grain at 0.06 opacity
- One ink: flag orange, used as marks and edges, never as glow or fill-for-mood
- Zero border-radius anywhere; hairline rules instead of cards
- Condensed variable Archivo for everything except measured values, which are JetBrains Mono with tabular figures
- Exactly one authored motion moment; all other movement is state transition
- Real pixels for evidence — no CSS approximation of a screenshot

## Colors

A monochrome ground and one saturated ink; every colour on the page is either a step of warm-neutral grey on near-black or the flag.

### Primary
- **Flag Orange** (`{colors.flag}`): the single ink. It draws the comparison seam and its grip, the left rule of every provenance caption and tick, list ticks, readout keys, section-label headings, the FAQ's drawn cross, focus outlines, selection background, caret, and the fill of the one primary download button. It is the page's only hue.
- **Flag Lift** (`{colors.flag-lift}`): the hover state of a flag-filled button only. Not a second accent; it exists so a filled button can respond without changing hue family.
- **Flag Wash** (`{colors.flag-wash}`): a 9%-alpha orange field behind the single binding notice (community project, unsigned runtime). The one place the ink becomes a field rather than a mark, because the notice is legally load-bearing.
- **Ink on Flag** (`{colors.ink-on-flag}`): the near-black brown used for text sitting *on* orange — primary button, skip link, selected text. Never pure black; it is warmed toward the flag.

### Neutral
- **True Black** (`{colors.ground}`): the page ground and the `theme-color`. Pure enough that OLED reads it as off, warm enough by a single blue step that it is not clinical.
- **Paper** (`{colors.text}`): default body and headline text. Slightly warm off-white, never #fff.
- **Paper Dim** (`{colors.text-dim}`): body copy inside sections, lede, nav at rest, list items, readout values. The workhorse reading colour.
- **Paper Meta** (`{colors.text-meta}`): provenance, captions, filenames, checksums, footer fine print. The quietest legible step.
- **Hairline** (`{colors.line}`): structural rules — section boundaries, the hero's bottom edge, ghost-button and CTA borders, `kbd` keys.
- **Hairline Soft** (`{colors.line-soft}`): rules *inside* a structure — between readout rows, between beats, between matrix groups, around figure images.

### Named Rules

**The One Ink Rule.** The page has exactly one hue. Anything that needs to be distinguished from its neighbour is distinguished by weight, width axis, greyscale step, or a drawn mark — never by introducing a second colour. A new state does not earn a new colour.

**The Mark, Not A Hue Rule.** State is expressed as a mark, not as a tint. The comparison grip carries two orange ticks like a splice mark; a list item carries a 6×1px orange tick instead of a bullet; an expanded FAQ row loses one arm of a drawn cross. When you reach for a coloured background to say "active", draw a rule instead.

**The No Glow Rule.** The ink never blooms. There is no `box-shadow` in orange, no radial gradient, no filter glow, no orange-tinted surface outside the one binding notice. Orange appears at full saturation, at hairline-to-2px scale, or as a solid button fill.

## Typography

**Display / Body Font:** Archivo variable (self-hosted woff2, weight 400–900, width 62–125%), falling back to system-ui
**Label Font:** the same Archivo, driven to the condensed end of its width axis (`'wdth' 78`) and set in uppercase
**Data / Mono Font:** JetBrains Mono variable (self-hosted woff2), falling back to ui-monospace

**Character:** One condensed grotesk doing all the talking, tightened as it gets bigger — headlines at `wdth` 92–94 with strongly negative tracking, labels crushed to `wdth` 78 with wide 0.15em letter-spacing. Mono is not a second voice; it is a notation, reserved for figures that were actually measured.

### Hierarchy
- **Display** (800, `clamp(2.7rem, 6.2vw, 5.1rem)`, line-height 0.96, `wdth` 92): the hero headline, once per page. Balanced wrapping, tracked in to -0.038em so the two lines read as a single block over the frame.
- **Headline** (750, `clamp(1.65rem, 3.1vw, 2.6rem)`, line-height 1.08, `wdth` 94): every `<h2>` section title.
- **Title** (700, 1.3rem, -0.02em): the per-step `<h3>` inside "How it works", and the requirements heading at a slightly smaller 1.05rem.
- **Body** (400, 1.0625rem, line-height 1.62, `wdth` 100): all prose. Constrained to a 64ch measure in section notes, FAQ answers and requirement lists; 38ch in the narrow beat columns; 34ch in package summaries.
- **Label** (650, 0.7rem, 0.15em, uppercase, `wdth` 78): the shared small-caps voice — nav links, comparison tags, readout keys, matrix group titles, package kind, footer column headings, the drag hint, and the meta line under a button label.
- **Data** (400, 0.76rem, tabular-nums, -0.01em, mono): provenance captions, per-step ticks (0.68rem), filenames, checksums (0.72rem), footer meta.

### Named Rules

**The Measured-Value Rule.** JetBrains Mono appears only where a value was measured, generated, or is literally a filename, a hash, or a keyboard key. Resolutions, frame counts, timestamps, dB figures, driver versions, package names, SHA-256 digests: mono. A sentence, a heading, or a label: never mono, no matter how technical it sounds.

**The Width-Axis Rule.** Hierarchy is carried by Archivo's width axis alongside its weight. Larger type gets narrower and tighter (`wdth` 92–94, negative tracking); smaller type gets narrower still and wider-tracked (`wdth` 78, +0.15em, uppercase). Body prose sits at the neutral `wdth` 100. Do not add a second family to make a new level.

**The No Eyebrow Rule.** Uppercase condensed type is a label attached to a thing — a key in a readout, a group title over a list, a tag on a frame edge. It is never a decorative kicker floating above a headline to announce a section.

## Layout

A single column of full-width sections on a true-black ground, centred at `max-width: 76rem` (widened to `84rem` for the "How it works" band, which carries screenshots) with a fluid gutter (`{spacing.gutter}`). Vertical rhythm comes from one large section pad (`{spacing.section-block}`), halved at the top of a section that directly follows another so two pads never stack, and hairline rules; there is no card grid and no background-tone change between sections.

The hero is the exception: `min-height: 100svh`, the comparison figure absolutely positioned full-bleed behind the type, a two-layer scrim (diagonal plus bottom) applied equally to both plates so the comparison stays fair, and the headline block held to `min(41rem, 92%)` over the frame's left third. Provenance packs the lower-left against an otherwise bare frame; the drag hint sits at the lower-right.

Internal structures are all rule-separated grids: the readout is a `7.5rem / 1fr` two-column key-value list with a soft rule under each row; "How it works" beats are an asymmetric `0.85fr / 1.25fr` grid that alternates sides by explicit `grid-column` placement (never `order`, so the screenshot always keeps the wide column); the format matrix and the download packages are `auto-fit` grids at `minmax(15rem, 1fr)` and `minmax(19rem, 1fr)`, sharing a top rule and giving each cell a soft bottom rule.

Two breakpoints, both editorial rather than device-shaped:
- **60rem** — the hero drops its full-height behaviour and its scrim and becomes a normal block: the comparison figure goes static with a 3:2 crop (taller than the 16:9 source so the face survives a phone's width) and its caption flows beneath. Beats collapse to one column, the readout stacks, and the drag hint is removed.
- **34rem** — the secondary nav links are hidden and only the primary Download link and the GitHub mark remain; buttons go full-width in a vertical stack so the primary action lands whole inside the first viewport.

## Elevation & Depth

**There are no shadows and no elevation.** Nothing lifts, nothing floats, nothing has a drop shadow. Depth is conveyed by exactly three devices: the fixed grain overlay that sits above everything at 0.06 opacity and unifies the surface; hairline rules at two strengths (structural 15%, internal 7.5%); and z-index stacking over the one photographic plate in the hero.

The four `shadow` declarations in the build are legibility and focus devices, not elevation, and should be read that way:

### Shadow Vocabulary
- **Tag legibility** (`text-shadow: 0 1px 6px rgba(0,0,0,0.8)`): only on text that sits on unpredictable footage — the ORIGINAL/NEURAL tags and the masthead links, which lie over the hero frame's brightest area.
- **Seam edge** (`box-shadow: 0 0 0 1px rgba(0,0,0,0.55)`): a dark 1px ring hugging the orange seam so it survives a bright frame. A hairline halo, not a shadow.
- **Focus halo** (`box-shadow: 0 0 0 3px rgba(5,5,6,0.9), 0 0 0 6px var(--flag)`): the keyboard-focus ring on the comparison grip, where the standard 2px orange outline would be lost on footage.

### Named Rules

**The Flat Ground Rule.** Every surface sits on the same plane. If something needs separation, it gets a hairline rule or whitespace. A `box-shadow` is only permitted to make a mark legible against photography, never to imply a layer.

**The Grain-Is-Material Rule.** The film grain is fixed to the viewport, above all content, non-interactive, at 0.06 opacity. It is a material from the subject's world, not a texture effect; do not animate it, do not vary its opacity per section, and do not add a second overlay on top of it.

## Shapes

Square. `border-radius` does not appear once in the stylesheet, including on buttons, the notice, the comparison grip, the play affordance, and the image frames. The form language is the rectangle and the line: 1px hairline borders, 1px–2px solid ticks and seams, 2px outlines on the two interactive marks (the comparison grip and the play frame, both 46–60px squares), and a 1px dashed bottom border as the only "dotted" texture, reserved for the copyable checksum.

Where other systems reach for an icon font or a glyph, this one draws the mark: the list bullet is a 6×1px rule, the FAQ toggle is a cross built from two 1px linear-gradient bars that loses its vertical arm when open, and the grip's two ticks read as a splice mark. The genuine icons on the page are an inline stroked SVG play triangle and GitHub's mark, which is a brand mark rather than an icon the page could draw.

## Components

### Buttons
- **Shape:** hard square (0 radius), 1px border always present — transparent on the primary so the box metrics match the ghost exactly.
- **Primary:** flag fill with warm near-black text, `0.85rem 1.5rem`. Stacked two-line content: a 700-weight 1rem label above a condensed uppercase meta line (version · size) at 72% opacity. The meta line is part of the component — a download button on this page always states what it will download.
- **Hover / Focus:** background lifts to Flag Lift over 0.16s ease. Focus is the global 2px flag outline at 3px offset.
- **Ghost:** hairline border, paper text; on hover both border and text go flag. In the hero it is the secondary action, **View source**, with the GitHub mark in its label and "GitHub · N stars" as its meta line, so it matches the primary's two-line shape and the pair is ranked by fill alone. In package form it is the secondary download and the source row.
- **Mobile:** below 34rem buttons become full-width and the action row stacks.

### Inputs / Fields
The only form control is the comparison range, and it is invisible by design (see Signature Component). There are no text inputs, selects, or checkboxes in this system; if one is added, it should be a bottom-hairline field on the ground with a flag focus rule, not a bordered box.

### Navigation
A baseline-aligned masthead absolutely positioned over the hero — no background, no blur, no sticky behaviour. Wordmark at left with "DLSS 5" in flag and the rest in paper, set at `wdth` 88 / weight 800. Links are Label-voice uppercase in Paper Dim, going Paper on hover, with a fluid gap. Below 34rem everything except the Download link and the GitHub mark is dropped rather than folded into a menu. The skip link parks *above* the viewport (not off to the left) because the page sets `overflow-x: hidden`.

### Readout
A key/value ledger under a structural rule. Flag-coloured uppercase Label key in a fixed 7.5rem column, mono tabular value in Paper Meta/Dim, one soft hairline per row, no zebra striping, no box. This is the page's canonical way to present measured fact at rest.

The **prose** variant (`readout__v--prose`) carries the limits and requirements: the same ledger, but its values are sentences, so they are set in the Body voice at 0.95rem, and only the measured figures inside them switch to the Data voice. It exists so the limits are stated with the same weight and shape as the evidence, not tucked into a footnote.

### Package Row
Not a pricing card: a rule-bottomed column in an auto-fit grid. Flag uppercase kind, dimmed summary held to 34ch, a CTA whose label and size sit on one baseline, the literal filename in mono below it, and a copy-checksum control. The pending variant greys the kind to Paper Meta and swaps the CTA for a link to the release page — a state change carried by greyscale and copy, not by a badge.

The **source** variant is the third column of the same row: kind "Source", the licence and what the code is for, a hairline CTA carrying the GitHub mark with "View source" over the star count (or the licence, when the build had no count), and the repository path in mono where a package names its file. It is the secondary action beside the downloads, in their shape, so it reads as an option rather than an advertisement.

### GitHub Link
The repository link appears in three places — the masthead, the source row beside the downloads, and the head of the footer — always as the same component: GitHub's own mark as an inline 16px SVG path in `currentColor` (no icon font, `aria-hidden`), the word, and the star count. The count is a measured value, so it is set in the Data voice (mono, tabular, Paper Meta) and parted from the label by a one-pixel hairline, never a pill or badge. It is fetched by `build.ps1` at build time only and its tooltip gives the build date; with no count the component simply omits it. Below 34rem the masthead keeps the mark alone, its label moved to the accessible name, with a 44px hit area grown by negative margin so the row does not move.

### Checksum Control
A `<button>` styled as text: mono at 0.72rem in Paper Meta, no background, a 1px dashed bottom border as its only chrome. On hover both text and border go flag; on successful copy the trailing uppercase micro-label swaps to "copied" in flag for 1.8s, then reverts. Confirmation is a word and a colour change on the control itself, never a toast.

### Disclosure (FAQ)
`<details>` rows separated by soft hairlines, summary at weight 650, native marker removed. The indicator is a drawn cross of two 1px gradient bars; open state removes the vertical bar and rotates 180°. No chevrons, no plus glyph.

### Signature Component: The Comparison Seam
Two real photographic plates at identical crop and `object-position`, stacked; the upper (original) is clipped by `clip-path: inset(0 calc(100% - var(--seam)) 0 0)`. A full-bleed `<input type="range">` at `opacity: 0` sits over the frame and mirrors its value into the `--seam` custom property, so the control is a genuine, keyboard-operable, screen-reader-labelled slider with a visible seam and grip drawn in CSS. The seam is 2px flag with a 1px dark ring; the grip is a 46px square with a 2px flag border, a 2px-blurred dark backdrop, and two inset flag ticks. Tags track the seam from the frame's top edge; a flag-ruled provenance caption anchors the lower-left.

**Authored position:** the seam settles at 55%, not 50% — at this crop the face occupies roughly 49–62% of the frame, so a centred seam would put the whole subject on one side and prove nothing.

**Motion:** on load, after a 550ms hold, the seam sweeps from 88% to 55% over 1500ms on an exponential ease-out (`1 - 2^(-10t)`), once, from an already-visible state. Suppressed entirely under `prefers-reduced-motion`, which also disables smooth scrolling and collapses all transitions to 0.001ms.

### Named Rules

**The One Moment Rule.** The page has exactly one authored animation: the seam's entry sweep. Everything else that moves is a state transition on hover, focus, or open, between 0.15s and 0.2s ease. No scroll-triggered reveals, no parallax, no staggered fade-ins. If a new element wants to animate, it is asking for the page's only moment and the answer is no.

**The Real Pixels Rule.** Evidence is shown at real resolution from real captures with intrinsic `width`/`height` attributes, and every shipping raster carries its provenance in an adjacent mono line (source resolution, frame, machine, capture settings). Never approximate a screenshot in CSS, and never ship an evidence image without its provenance.

## Do's and Don'ts

### Do:
- **Do** separate sections with a hairline rule (`{colors.line}` between structures, `{colors.line-soft}` within one) and generous block padding.
- **Do** keep every new element at 0 radius.
- **Do** set measured values in JetBrains Mono with `font-variant-numeric: tabular-nums`, and everything else in Archivo.
- **Do** attach provenance to any picture that functions as evidence, in the mono Data voice.
- **Do** express state as a drawn mark — a tick, a rule, a seam, a greyscale step.
- **Do** put the version and the file size on any download control, as its second line.
- **Do** build interactions on real, native controls (`<input type="range">`, `<details>`, `<button>`) and draw the visible affordance over them.
- **Do** keep the primary action whole inside the first viewport at 390px by tightening the stack, not by shrinking the type.
- **Do** respect `prefers-reduced-motion` by suppressing the seam sweep entirely, not by shortening it.

### Don't:
- **Don't** introduce a second hue. Flag orange is the only colour; greyscale steps and Archivo's width axis carry the rest.
- **Don't** let the ink glow: no orange shadows, no radial gradients, no tinted panels. The one orange field is the binding legal notice.
- **Don't** wrap content in a card, a panel, or a bordered container. The only bordered boxes on the page are buttons, image frames, and the notice.
- **Don't** set prose, headings, or labels in monospace. Mono is notation for measured values, filenames, hashes and keys.
- **Don't** add a decorative uppercase kicker or eyebrow above a headline. Uppercase condensed type must label a specific thing.
- **Don't** add an icon font or glyph stand-in (`+`, `→`, `▸`) where a mark can be drawn; if a true icon is unavoidable, inline a stroked SVG.
- **Don't** add scroll-triggered animation, parallax, or entrance staggering; the seam sweep is the page's only authored moment.
- **Don't** tint, dim, or wash over footage in a section whose job is to show image quality — put the label on a plate beside or beneath the frame instead.
- **Don't** reorder responsive columns with `order`; place them with `grid-column` so the wide column always keeps the screenshot.

## Player

Everything above is the project site. The native player (`src/`, Win32 + GDI
chrome over a D3D12 picture) is a separate surface with its own tokens, and
this section records them. The player is an **Operate** surface: the video is
the hero, the chrome recedes during playback, and nothing is ever drawn over
the comparison point. The one thing it shares with the site is the flag
orange, used the same way: as the mark under the selected compare mode.

### Tokens

The palette is `ui_palette` in `src/UiResources.h`; the button looks are
`ResolveButtonVisual` in `src/UiResources.cpp`.

| Token | Value | Use |
|---|---|---|
| Window | `RGB(18,19,21)` | Window and start-screen ground; dark label on lit pills |
| ControlSurface | `RGB(27,28,31)` | The control strip; the pressed button's sunken fill |
| Inactive | `RGB(47,49,53)` | Resting pill, chip and compare segment |
| Hover | `RGB(62,65,70)` | Hovered resting pill |
| PrimaryBlue | `RGB(55,139,226)` | On (active) pill, played progress; hovered `RGB(84,158,236)` |
| NeuralCoverage | `RGB(72,196,178)` | Working pill, rendered coverage lane; hovered `RGB(104,214,198)` |
| Coverage lit | `RGB(176,246,234)` | The coverage lane at the height of the render-complete glow |
| MarkedRange | `RGB(158,112,240)` | The In/Out selection (edge `RGB(206,178,255)`) |
| Attention | `RGB(255,168,64)` | A dropped frame; the Out marker |
| Flag | `RGB(255,106,26)` | The selected compare mode's 2 px mark, as on the site |
| PrimaryText / SecondaryText | `RGB(240,240,242)` / `RGB(160,164,172)` | Labels / quiet values and captions |

Type is Segoe UI at 16 px (body), 14 px (pills, chips, status) and 24 px
semibold (the start screen's title), all in DIPs through `ActiveWindowDpi`.
Icons are the bundled Tabler font (`GlyphForIcon`); without it every control
keeps its words. Pills have an 8 dip radius and a 36 dip minimum hit height;
chips a 4 dip radius; compare segments are square with a 1 px gap. A lit pill's
label must keep 4.5:1 contrast in every state, hovered included (PolicyTests
checks it).

### Motion

Motion is information: it says where something came from or went, and it
never decorates. The numbers live in `src/ChromeMotionPolicy.h`, with tests.

| Moment | Trigger | Duration and easing | Without animations (SPI_GETCLIENTAREAANIMATION off) |
|---|---|---|---|
| Hover tint on a toolbar control | cursor enters / leaves | in 120 ms cubic ease-out; out 180 ms cubic ease-in; a reversal starts from the current level | lands at once |
| Press | button down | no animation: glyph and label sink 1 px into the darker fill for as long as it is held | same |
| Render complete | a live session that had holes has none left | 900 ms: the coverage lane lights over the first fifth, a highlight crosses it left to right, then it eases back to teal | the lane holds lit for 900 ms, no sweep |
| Status chip flash | the fact a chip reports changes (not every repaint) | 900 ms linear fall-off (`status_chips::Flash`) | holds lit for 900 ms |
| Slider knob | pointer on a slider, or dragging it | knob 6 to 8 dip on the hover fade | lands at once |
| Volume value bubble | a volume drag | fades in with the knob; lingers 400 ms after release, then fades out | appears and goes, linger kept |
| Compare mark | the compare mode changes | slides from the old segment to the new over 160 ms, ease-out | jumps |
| Comparison tags | the compare mode changes | fade in over 120 ms, ease-out (the compositor's tag alpha) | there at once |
| Start tile lift | pointer on a tile | rises 2 dip, picture +6% brighter, blue edge, on the hover fade | lands at once |
| Toast | a file saved, a subtitle shift, a whole video rendered | takes the status row's slot: rises 8 dip and fades in over 160 ms (ease-out) as the line fades out, holds 2.4 s, then fades out over 140 ms (ease-in) as the line returns; repainted at 30 Hz; a second one replaces it without rising | the line and the toast swap for the hold |
| Rendering-now hatch | a live job is running | drifts with the 50 ms activity timer | still hatch, 1 s repaint |

Rules:
- 120-220 ms for anything that answers the pointer; ease-out when something
  arrives, ease-in when it leaves; no bounce, no overshoot.
- A timer runs only while something is moving, and it repaints only the
  rectangle that moves; a paint draws only what that rectangle reaches.
  Moments that arrive during playback (the glow, a toast) repaint at 30 Hz. Chrome animation must never cost a video frame:
  the playback-health line (`Playback health: ... dropped=`) is the measure.
- Every animation has a static equivalent that says the same thing.
- Nothing flashes over the picture.

### Components

- **Slider** (`src/SliderPolicy.h`): one look everywhere - the strip's
  volume, the compare bar's Mix and every settings-dialog trackbar. A 4 dip
  rounded rail in `RGB(68,71,77)`, the stretch from the origin to the value
  filled in PrimaryBlue, a round knob in `RGB(246,246,248)` with a Window
  rim. A level fills from its start; a setting with a neutral point (the Mix,
  brightness, contrast, the neural look) fills from that point, so a
  control at its default shows no fill. A value bubble appears over the knob
  only where it cannot cover the picture (the volume); the Mix's value is
  already read out beside its track.
- **Toast**: an Inactive-grey panel with a 4 dip radius and a 3 dip mark on
  its left edge (teal for a render, the accent otherwise), in the status row's
  slot: the line crossfades out as the toast comes in and back as it goes, so
  no part of the line shows behind or after it (without animations they swap). It is painted by the strip, never
  as a window over the picture: a layered popup over the swap chain cost
  frames (6, 6 and 2 dropped at a render's completion, against 0, 0 and 1).
  The status line keeps the same notice after it goes.
- **Dialog group heading**: Segoe UI 11 dip semibold, upper case, tracked
  1 dip, in SecondaryText, with a `RGB(62,65,70)` hairline to the column's
  edge. It labels a group; it is never a decorative kicker.
- **Dialogs** fit a 1080p screen at 175%: at most 1920x1032 px including the
  frame, which is why Neural settings is two columns.

### Tooltips

Every toolbar control has a tip (`src/ToolbarTipPolicy.h`). A control with a
key names it on the first line as `Name (Key)`, the form the compare bar
uses, and the key is checked against the control's menu row by PolicyTests.
A second line, when there is one, says what the control does in the plain,
measured voice the rest of the product uses; where there is a measurement,
the tip quotes it (Upscaling, Mix, SR history). Tips appear after 500 ms and
at once when moving from one control to the next. The status chips and the
compare modes build theirs when shown (TTN_GETDISPINFO): a chip says what
was measured and over what; a greyed mode says what would make it work.

### Keys

`Esc` leaves fullscreen before anything else, as in every other player; in a
window it stops the longest-running job (`src/EscapeKeyPolicy.h`).
