# Player and site UX audit, 2026-09-24

This covers the native player (Win32 + GDI chrome over a D3D12 picture) and
the project site (`site/`). It records what is good, where people get stuck,
what is missing, and a plan of 18 improvements in three rounds. All three
rounds shipped in `main`; the round sections below record what was built, and
what was cut.

## How this was done

- **Machine:** RTX 4080 SUPER, driver 610.47. The primary monitor is 3840x2160
  at 175% (168 dpi), and the second is 1920x1080. The player opens at a
  1440x880 client, which is 823x503 dip.
- **Player:** `build\Release\DLSSVideoPlayer.exe` from this worktree, with the
  runtime staged. The input was the fixture
  `tools/benchmark/fixtures/demo-capture-20260912.mp4` (1920x1080, 30 fps,
  22.6 s, no audio), starting from a fresh profile. The player was driven
  with posted window messages (the session locked part-way through) and
  captured with `PrintWindow(PW_RENDERFULLCONTENT)` by window handle, the same
  method as `tools/verification/capture-window.ps1`.
- **Surfaces captured:** start screen, playing, paused, the shortcut sheet,
  the compare bar (inert and live), the loupe, the Image adjustments, Neural
  settings and Encoder settings dialogs, fullscreen, a whole live neural
  session (D, the warm-up, a seek during the warm-up, attach, the second hole,
  completion), and the toolbar's rest, hover and press states.
- **Site:** built offline from `site/fixtures/release-full.json`, served
  locally, and captured with Playwright at 1440x900 and 390x844. The
  Impeccable detector was run over `site/src` (degraded regex mode, because
  its parser modules are not installed).
- The screenshots are in `C:\t\w6-design\shots\` (`before-*`, `after-*`). They
  are not committed.

## What is good, and should be kept

- **The coverage model is visible.** The timeline carries three lanes that do
  not hide each other: the In/Out selection, played progress, and rendered
  coverage. There is also the hatched "rendering now" stretch. No other player
  shows where a render is and where it is growing.
- **Honest states.** A pill has four states (unavailable, off, on, working),
  and each has its own look. A press during a seek says "Queued for the
  seek". The upscaling pill gives the reason it is unavailable, not just
  "Unavailable". The chips flash only when the fact they report changes, not
  on every repaint.
- **Measured voice.** Tooltips and menu rows quote measurements, such as SR
  history VMAF ranges, processing-scale render rates and the preset timing.
  The start screen says "about 65 fps at 1080p" for this GPU.
- **Dark and DPI-correct throughout.** The menus, dialogs, the shortcut sheet
  and the compare bar are all dark and all scale with dpi. The compare bar
  steps down through four tiers instead of clipping.
- **The compare bar borrows the site's mark.** The selected mode has a 2 px
  flag-orange rule under it, not a filled pill. That is the one ink the
  product and the site share.
- **Reduced motion is already a first-class input.** `ReadAnimationPreference`
  reads SPI_GETCLIENTAREAANIMATION, and the chip flash and the activity
  spinner already have static equivalents.
- **The site** is a strong, specific design system: true black, one ink,
  hairline rules, and provenance on every image. The detector found only
  advisory type-ramp drift (16 sizes off the documented ramp). The comparison
  is a real `<input type="range">`.

## Friction, with evidence

Severity follows Impeccable's scale: P0 blocks a task, P1 is a major
difficulty, P2 is an annoyance with a workaround, and P3 is polish.

| # | Sev | Where | What happens | Evidence |
|---|---|---|---|---|
| F1 | P1 | Toolbar tooltips | At the default size the bar is icon-only, and Back 10 s, Stop, Forward 10 s and the start screen's "Open YouTube URL" had **no tooltip**. The Fullscreen tip said "Escape or F leaves", but **F previews a frame and starts a neural render**, and F11 is the key. No tip named its shortcut. | `before-03-paused.png`; `src/main.cpp` `ToolbarTipKey` (before `72d9469`) |
| F2 | P1 | Timeline, live warm-up | A seek during a live session's warm-up painted played progress over the hatched "rendering now" stretch. The only sign of where the render was working vanished during the ~20 s wait it explains. | `lane-before-after.png` (rows 1 and 3) |
| F3 | P2 | Toolbar | Hover snapped on and off, and a lit (on or working) pill **did not react to hover at all**. On the icon-only bar, the one control that was on was the one that looked dead under the pointer. | `native_button_palette...` test pinned `activeHover == active` |
| F4 | P2 | Render completion | A render that took minutes ended with the chip reading "Render 100%", and nothing else marked the moment. | `before-n8-late.png` |
| F5 | P1 | Live warm-up honesty | During the cold start the chip read **"Render 0% · ETA 0:10" for about 20 s** without changing. The status line was cut off at "Buffering neural frames · 0.0 s bu…". The measured cold start was 21.9 s (request 8.1 s for the model-store hash, preflight 3.1 s, feature arm 2.9 s, attach 4.5 s) on a fresh profile, and 16.9 s once the store was hashed. | `before-n1-3s.png`, `before-n4-after-seek.png`; the log's `Neural cold start:` line |
| F6 | P2 | Default window | At 175% the default 823x503-dip window drops every pill to an icon. The feature pills' state words ("On", "Preparing cache", "No cache") exist only in the tooltip and the colour. | `before-03-paused.png` |
| F7 | P2 | Fullscreen | The controls hide by resizing the picture (`Layout()` gives the strip's height back to the viewport). The picture jumps and the swap chain resizes, so the hide pops. The brief asks for a fade and slide. | `AutoHideFullscreenControls`, `Layout` |
| F8 | P2 | Esc in fullscreen | During a live session, Esc in fullscreen **stops the render** and does not leave fullscreen. This is documented (USAGE.md "an active render or download consumes Esc to cancel first"), but every other player uses Esc to leave fullscreen first. | `WM_KEYDOWN` Esc order |
| F9 | P2 | Volume slider | The track is a flat grey line with a knob. There is no filled portion to the level, and no hover or drag affordance. The only value readout is the "Vol 100%" label. | `before-03-paused.png` |
| F10 | P3 | Confirmations | "Comparison saved: <path>" and similar confirmations appear in the status line, which is already crowded and cut off, so they are easy to miss. There is no toast. | `compare.save.saved` |
| F11 | P3 | Compare bar | The segments have no hover fade. The orange mark jumps between modes instead of sliding. While there is no neural pair, the segments are greyed with no tip to say why ("Comparison mode refused: cachedPair=0" appears only in the log). | `before-05-compare.png` |
| F12 | P3 | Chips | The chips carry measured facts but have no tooltip. The signature measured voice stops at the bar. | |
| S1 | P2 | Site, 390 px | The inline plate links ("original" and "neural" under each gallery plate) are 48-58x12 px, and the masthead Download link is 68x18. All are below the 24x24 minimum target (WCAG 2.5.8). | Playwright measurement |
| S2 | P3 | Site gallery | The first gallery plate (a 16:9 frame) sits in a frame sized for the 1152x744 player captures, so it is letterboxed with black bands. The player captures in that section still show the old light title bar and menu. | `site-before-gallery.png` |
| S3 | P3 | Site CSS | 16 font sizes are off DESIGN.md's type ramp (detector, advisory). | `detect.mjs` |

## What is missing

- A **toast** layer for short confirmations (saved, copied, rendered, subtitle
  delay), rising from the strip.
- **Tooltips on the chips and the timeline** in the product's measured voice.
- A **fullscreen overlay strip** that can fade over the picture instead of
  resizing it.
- **Slider affordances** (a filled track, a knob that grows on hover or drag,
  and a value bubble while dragging) on the volume slider and the Mix track.
- An **explained warm-up**: which step of the cold start is running, instead of
  a frozen percentage.

## Site audit summary

| Dimension | Score | Key finding |
|---|---|---|
| Accessibility | 3 | Small inline link targets at 390 px (S1); the comparison is keyboard-operable and labelled |
| Performance | 3 | AVIF plates with intrinsic sizes and lazy loading; the demo loads on request |
| Responsive | 3 | Two editorial breakpoints hold; no horizontal scroll at 390 px |
| Theming | 4 | Tokenised, one ink, consistent |
| Implementation integrity | 4 | A coherent, product-specific system; only advisory type-ramp drift |
| **Total** | **17/20** | **Good**: address S1, then polish |

## The plan

Each item gives the user moment it serves, the interaction or motion spec, and
how it is built. Motion numbers live in `src/ChromeMotionPolicy.h` and the
DESIGN.md "Player" section.

### Round 1: built (tier 1)

1. **Tooltips that teach** (F1): `72d9469`.
   - *Moment:* someone meets an icon-only bar for the first time.
   - *Spec:* every control has a tip with the first line `Name (Key)`. It
     appears after 500 ms, or at once when moving between controls, and
     stays up for 30 s. Nothing here animates, so reduced motion needs no
     separate behaviour.
   - *Build:* `ToolbarTipPolicy.h` holds the keys, the menu row each key must
     match, and the parsers. The tip rectangles follow the surface, and a
     control that is not shown gets an empty rectangle.
   - *Tests:* PolicyTests cross-checks every tip's key against its menu row.
     PlayerUiRegressionTests checks that the rectangles follow the start
     screen and the loaded bar, and checks the delays.
2. **Hover and press micro-interactions** (F3): `1a199b3`.
   - *Moment:* moving along the bar and pressing a control.
   - *Spec:* the hover tint eases in over 120 ms (cubic ease-out) and out over
     180 ms (cubic ease-in). A reversal starts from the current level. A lit
     pill lifts a step toward white in its own hue, and its label contrast
     only rises. A press sinks the glyph and label by 1 px. With reduced
     motion the tint lands at once.
   - *Build:* `chrome_motion::Fade` for each action, blending the resting and
     hovered `ResolveButtonVisual` looks. A 16 ms timer repaints only the
     buttons that are moving and stops itself when they land.
3. **Keep the render lane clear during the warm-up** (F2): `4000761`.
   - *Moment:* seeking while the first render warms up.
   - *Spec:* the bottom lane of the track always belongs to the render map
     during a live session.
   - *Build:* a single condition in the timeline painter, with a pixel test.
   - *Seen live:* `lane-before-after.png`.
4. **The render-complete moment** (F4): `f3ec093`.
   - *Moment:* the last hole of a live render fills.
   - *Spec:* the whole effect lasts 900 ms. The lane lights over the first
     fifth (ease-out), a highlight crosses it left to right (ease-out, gone by
     70%), and the lane then eases back to teal (ease-in). It fires once per
     session, and only for a session that was seen with holes. With reduced
     motion the lane holds lit for 900 ms with no sweep. There is no sound and
     nothing over the picture.
   - *Build:* `chrome_motion::Glow` and `CompletionLatch`, observed with the
     chips, on a 900 ms timer.
   - *Seen live:* `after-glow-montage.png`. Playback health was 29.97/30 fps
     with 0 dropped across the glow.
5. **Record the player's design tokens and motion spec** in DESIGN.md
   "Player": a separate docs commit.

### Round 2: next tier

6. **Toasts that rise from the strip** (F10, F4).
   - *Moment:* confirming that a file was saved, a subtitle shift, or a whole
     video rendered ("Whole video rendered · 22.6 s in 0:14").
   - *Spec:* the toast rises 8 dip and fades in over 160 ms (ease-out) from
     the strip's top-left, holds for 2.4 s (longer while the pointer is over
     it), then fades out over 140 ms (ease-in). At most one toast is shown,
     and a new one replaces the old in place. With reduced motion it appears
     and disappears with no rise and no fade.
   - *Build:* a layered popup owned by the main window (`UpdateLayeredWindow`
     with per-pixel alpha), the same kind of surface as the buffer overlay.
     It is never over the compare seam: it anchors above the strip at the
     left. Timing goes in a `ToastPolicy` inside `ChromeMotionPolicy.h`.
7. **An honest warm-up** (F5).
   - *Moment:* the 17-22 s between pressing D and seeing the first rendered
     frame.
   - *Spec:* the Render chip reads "Render · starting" until a pace is
     measured. It never shows an ETA made up before then (today it shows
     "ETA 0:10"). The status line names the step: checking the neural
     runtime (first run), preparing the model, rendering the first second.
     There is no new motion; the existing spinner and hatch carry it.
   - *Build:* a `status_chips::Build` arm for "pace unknown", driven by the
     cold-start phases the log already records.
8. **Volume and Mix sliders with affordances** (F9).
   - *Spec:* the track fills to the level in PrimaryBlue. The knob grows from
     5 to 7 dip on hover and while dragging (120 ms ease-out in, 180 ms
     ease-in out). A value bubble ("62%") sits above the knob while dragging,
     fades in over 120 ms, and fades out 400 ms after release. With reduced
     motion the size change and the bubble have no transition.
   - *Build:* the same `Fade`, and a `slider` hit rectangle for hover.
9. **Tooltips in the measured voice on the chips and the timeline** (F12).
   - *Spec:* the Render chip gives frames rendered, segments, and pace against
     real time. The fps chip gives what was presented over the last
     measurement window. The Dropped chip says when the last frame was late
     and why (for example "at the session start"). They use the same delays
     as the toolbar tips.
   - *Build:* rectangle tools for each chip, re-registered with the chip
     layout, with text built from `status_chips` and the playback-health
     numbers.
10. **Compare bar polish** (F11).
    - *Spec:* segments get the same hover fade. On a mode change the orange
      mark slides from the old segment to the new one over 160 ms
      (ease-out). While there is no neural pair, a greyed segment's tip says
      why ("Needs rendered frames: press D"). With reduced motion the mark
      jumps.
    - *Build:* a `Fade` for each segment, and a `Slide` value for the mark in
      `ChromeMotionPolicy.h`.
11. **Start screen tiles and empty state.**
    - *Spec:* a tile lifts on hover. Its border lightens and its thumbnail
      brightens by 6% over 120 ms. The hint line under the tiles becomes an
      invitation with the key ("Drop a video here, or press Ctrl+O"). With
      reduced motion the lift has no transition.
    - *Build:* `DrawStartTile` with a `Fade` for each tile.
12. **Timeline hover.**
    - *Spec:* the track thickens by 2 dip and the knob grows while the pointer
      is over it (120 ms ease-out, 180 ms ease-in). The thumbnail preview
      fades in over 120 ms. With reduced motion none of these animate.
    - *Build:* the existing `UpdateTimelineHover`, plus a `Fade`. The preview
      window already exists; its fade uses a layered alpha.
13. **Site tap targets** (S1).
    - *Spec:* the inline plate links and the mobile masthead Download link
      get padding up to a 24x24 target without changing their visual size.
    - *Build:* CSS only. `site/test.ps1` stays green.

### Round 3: final set

14. **Fullscreen overlay strip** (F7). In fullscreen the picture keeps the
    whole screen, and the strip is a layered, owned popup over its bottom
    edge. It fades in over 160 ms with a 12 dip rise and fades out over
    220 ms (ease-in) after the idle delay. With reduced motion it
    appears and disappears at once. This is the highest-risk item: the strip
    is painted in the main window today, so it has to move to a layered
    surface without costing video frames. **If it is not clean by round 3,
    it is cut and F7 is documented instead.**
15. **The Esc order in fullscreen** (F8): a decision for the lead. The
    proposal is that Esc leaves fullscreen first, and a second Esc stops the
    render. USAGE.md and the shortcut sheet change with it.
16. **Default window size** (F6): a decision for the lead. Open at 70% of the
    work area, so that at 175% the pills keep their words.
17. **The render-band growth.** When a segment lands, the coverage band's
    edge eases to its new position over 180 ms instead of stepping. With
    reduced motion it steps.
18. **Final visual tour** of every surface, the reduced-motion pass, and the
    playback-health comparison with and without chrome animation.

## The live neural session (the refactored job code)

The session ran twice: once on the baseline, and once on this branch with a
cold render cache. D was pressed on the fixture, the video played, there was
one seek (+10 s) during the warm-up, then Split, the loupe, and a run to the
end. Nothing went wrong in the neural-job code:

- Each time the session attached, then published run 1 with every frame
  verified (662/662 on the baseline, 652/652 on this branch; receipt
  `failure=none`). It rendered the remaining hole at the start as run 2 on
  the reused helper (`helper=reuse`), and logged "rendered all of
  [0,22.6) s". Dropped frames were 1 at the session start on the baseline and
  0 on this branch. Every playback-health line after the attach read
  29.9-30.0 of 30 fps.
- The seek to 10.55 s during the warm-up did **not** move the render: the job
  kept rendering from 0.53 s and attached when it reached 10.57 s. This is by
  design (`live_session::ShouldRetarget`: a target within `kRebaseAhead`
  = 15 s of the job's reach is waited for, not restarted), but it is what
  makes F2 and F5 matter, because the wait after that seek was about 12 s.
- The cold start on a fresh profile took 21.9 s, of which 8.1 s was the
  model-store hash of 59 files. With the store already hashed it took
  16.9 s. The warm-up UI does not say this (F5).

## Round 2: what was built

All of the round-2 plan (items 6-13) shipped, together with six review
additions:

- Esc leaves fullscreen first.
- The first window is 80% of the work area.
- The live render's cold start is explained, and measured.
- The comparison tags fade in.
- The dialogs are redesigned.
- The volume slider sits on the button row.

The chrome also gained a frame budget. A toast drawn as a popup over the video
cost frames, so the toast now lives in the status row, paints draw only what
changed, and the moments that arrive during playback repaint at 30 Hz. The
round-2 section of the w6-design report has the details.

## Round 3: final

### What changed

| Change | Commit |
|---|---|
| Rebased onto `improve/audit-2026-09-23` (site rewrite, direct NVENC encode, P3.4, 682322a); the 24 px tap-target rules survived next to the new masthead GitHub link | the rebase |
| **Cold start (Option A).** A just-rewritten model-store file is trusted when its size and hash equal the last agreed pair's (`ModelStoreMemo`, persisted beside the cache). Measured on the fixture: model-store resolve 9.7-10.5 s → 0.07-0.43 s; cold start 17.9-21.9 s → 7.9-9.0 s. | `87b3efd` |
| The first warm-up step now reads "waiting for NVIDIA's model files to settle" | `0d2288a` |
| The dialog heading rule clears its text at 100/150/175% (pinned by a pixel test) | `467ae4b` |
| The status-line slot crossfades with a toast (no text behind or after it) | `f580a91` |
| Window placement is saved and restored, validated against the current monitors | `c913980` |
| The render band grows as segments land (180 ms, ease-out) | `51fedda` |
| **The 8-frame drop at publish** was a player-side starvation. The joined entry's open was still in flight at the next boundary. The warmed next file is now kept through the join; measured over 8 completions: 0 dropped. | `77156c9` |

### Cut: the fullscreen fade-and-slide strip

The strip is cut, on measurement. The controls sit below the picture in
the main window. Easing them would mean either resizing the swap chain every
animation frame, or drawing the strip as a layered popup over the video.

The popup was built and measured in round 2, as the first version of the
toast, on the fixture's render completion:

| Version | Dropped frames, 3 runs |
|---|---|
| Popup over the video | 6, 6, 2 |
| No toast | 0, 0, 1 |
| Toast painted by the strip | 0, 0, 0 |

No zero-cost path was found. What would make the strip possible is composing
the chrome through DirectComposition, as a visual beside the swap chain that
the compositor animates. It would have to pass the same bar: an unchanged
playback-health line while it animates. DESIGN.md "Player > Fullscreen"
records this.

### Also out of scope and not built

The loupe could not be captured this round: posted mouse input cannot hold the
pointer over the picture. It is unchanged since round 1 and covered by the
existing tests.

### Before / after gallery

All screenshots are in `C:\t\w6-design\shots\`, captured on the RTX 4080 SUPER
at 3840x2160 and 175%. Before shots are round 1's `before-*`; after shots are
round 3's `f3-*`.

| Surface | Before | After |
|---|---|---|
| Start screen | `before-01-start.png` | `f3-01-start.png`; `r2f-02-start-tilehover.png` (new subtitle) |
| Playing, first window | `before-02-playing.png` (1440x880 px client, icon-only pills) | `f3-02-playing.png` (2293x1563 client, pills with their words) |
| Warm-up | `before-n1-3s.png` ("0% · ETA 0:10" frozen) | `f3-03-warmup.png` ("Starting the neural render · preparing the model on the GPU", "Render · starting") |
| Timeline during a render | `before-n4-after-seek.png` (lane overpainted) | `f3-04-rendering.png`; `lane-before-after.png` |
| Completion | `before-n8-late.png` (a chip reading 100%) | `f3-05-complete-*.png` (glow and toast), `f3-strip-sheet.png` |
| Compare modes | `before-05-compare.png` (refused before a pair) | `f3-06-mode-{neural,original,split,wipe,difference,side,quad}.png`; `f3-modes-sheet.png` |
| Tags | — | `r2-tags-split.png`, `r2-tags-split-early-top.png` (fade) |
| Toast | — | `f3-08-toast.png`, `f3-08-toast-gone.png` |
| Volume slider | `before-03-paused.png` | `r2a-volume-drag.png` (bubble), `f3-02-playing.png` (on the row) |
| Image adjustments | `before-07-dlg-adjust.png` | `f3-09-dlg-adjust.png` |
| Neural settings | `before-07-dlg-neural.png` (1,387 px) | `f3-09-dlg-neural.png` (796 px, two columns) |
| Encoder settings | `before-07-dlg-encoder.png` (combo and note cut off) | `f3-09-dlg-encoder.png` |
| Shortcut sheet | `before-04-sheet-popup.png` | `f3-10-sheet.png` (Esc row updated) |
| Placement restored | — | `f3-11-reopened.png` (moved to 300,150 2100x1400, reopened there) |
| Site | `site-before-desktop-full-0.png`, `site-before-mobile-full-0.png` | `f3-site-desktop-hero.png`, `f3-site-mobile.png` |

A capture caveat: PrintWindow sometimes catches a directly painted popup (the
sheet, a dialog) part-way through a repaint. The gallery keeps complete
captures. The paint code itself validates and double-checks were complete.
