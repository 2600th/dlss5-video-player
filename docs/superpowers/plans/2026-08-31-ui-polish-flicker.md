# Native UI Polish and Flicker Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the existing Win32 player clearer, more polished, and visibly flicker-free while keeping video dominant and retaining the native architecture.

**Architecture:** Extract only pure toolbar layout/hit-testing, embed a pinned Tabler icon font and app icon, then convert the existing paint path to one memory-buffered blit with targeted invalidation. Preserve the current dark visual identity, refine its hierarchy, and add explicit empty/loading/error/status states.

**Tech Stack:** C++20, Win32/GDI, Windows resources, CMake/CTest, Tabler Icons Webfont 3.46.0.

**Spec:** `docs/superpowers/specs/2026-08-31-release-ui-youtube-design.md`

## Global Constraints

- Execute after `2026-08-31-runtime-policy-localization.md` in the same isolated worktree.
- Use `superpowers:test-driven-development` for layout/hit-testing and `superpowers:systematic-debugging` for any remaining flicker.
- Apply the `impeccable` Operate-mode quality bar: primary task/state first, complete interaction states, coherent spacing, visible keyboard access, no decorative animation.
- Immediately before editing UI code, read `impeccable/reference/craft-floor.md` as required by that skill.
- Preserve native Win32/GDI/D3D12; do not add a UI framework, retained scene graph, browser control, or new dependency manager.
- Preserve the existing dark palette unless contrast measurement requires a narrow adjustment.
- Do not run the known-unsafe 4K re-hook stress path.

---

## Task 1: Pin and embed the icon assets

**Files:**
- Create: `tools/fetch_ui_assets.ps1`
- Create: `assets/tabler/tabler-icons.ttf`
- Create: `assets/tabler/tabler-icons.css`
- Create: `assets/tabler/LICENSE`
- Create: `assets/tabler/SOURCE.txt`
- Create: `assets/DLSSVideoPlayer.ico`
- Create: `src/resources.h`
- Create: `src/resources.rc`
- Modify: `CMakeLists.txt`
- Modify: `THIRD_PARTY.md`

- [ ] Write `fetch_ui_assets.ps1` to download only `https://registry.npmjs.org/@tabler/icons-webfont/-/icons-webfont-3.46.0.tgz`, verify npm SRI `sha512-aQouIxJQb+F5cRsHo/FW5qSILDuU7pd7d86JjmSUCMgpJhBeRuyovnfJlQeyuug2gHz0jFaosl4GNYo3SLnjrQ==`, extract the font/CSS/license to a temporary directory, and copy only the named committed assets.
- [ ] Run the script once, inspect source metadata, and record package/version/source/SRI in `SOURCE.txt`.
- [ ] Derive the multi-size application icon from Tabler's `sparkles` or `wand` glyph with a restrained blue-on-charcoal treatment; include 16, 24, 32, 48, and 256 px images in the ICO.
- [ ] Use `src/resources.h` IDs and `src/resources.rc` to embed the font bytes and application icon; set both `hIcon` and `hIconSm` when registering the window class.
- [ ] Add the `.rc` file to the app target and add Tabler's MIT notice to `THIRD_PARTY.md`.
- [ ] Verify `Get-Item build\Release\DLSSVideoPlayer.exe` succeeds after a Release build and Windows Explorer shows the custom icon.
- [ ] Commit:

```text
feat: embed Tabler UI resources
```

## Task 2: Create pure toolbar layout and hit testing

**Files:**
- Create: `src/UiLayout.h`
- Create: `src/UiLayout.cpp`
- Modify: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Write failing tests for narrow, normal, and wide client widths; group separation; minimum 36-DIP hit height; no overlapping hit rectangles; and stable hover identity at boundaries.
- [ ] Define the public types:

```cpp
enum class ToolbarAction {
    Open, Back10, PlayPause, Stop, Forward10, Mute,
    ToggleDlss, Aspect, Adjustments, DebugView, Fullscreen, None
};

struct ToolbarItem {
    ToolbarAction action{ToolbarAction::None};
    RECT bounds{};
    bool compact{false};
};

std::vector<ToolbarItem> LayoutToolbar(int clientWidth, int clientHeight, UINT dpi);
ToolbarAction HitTestToolbar(std::span<const ToolbarItem> items, POINT point);
```

- [ ] Confirm RED before adding `UiLayout.cpp`.
- [ ] Implement a 4-DIP spacing unit, 36-DIP minimum hit height, 8-DIP corners, and 16-DIP outer gutter; use larger separators between Playback, Enhancement, and View groups.
- [ ] At narrow widths, keep Open, Play/Pause, Mute, DLSS, and Fullscreen visible and collapse secondary actions into an existing/native overflow menu instead of shrinking targets below 36 DIP.
- [ ] Keep action order and keyboard commands stable.
- [ ] Add the module to app and test targets, run CTest, and commit:

```text
refactor: extract toolbar layout
```

## Task 3: Introduce icon-assisted control rendering

**Files:**
- Create: `src/UiResources.h`
- Create: `src/UiResources.cpp`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`

- [ ] Implement RAII loading with `FindResourceW`/`LoadResource`/`AddFontMemResourceEx`; create a Tabler font at the active DPI and release it with `RemoveFontMemResourceEx`.
- [ ] Map only the glyphs used by the player: folder/open, rewind, play, pause, stop, fast-forward, volume, volume-off, sparkles, crop, adjustments, bug/debug, maximize, YouTube, and warning.
- [ ] Refactor `DrawButton` to accept action, icon, short label, enabled, active, hover, pressed, and focus states; icons must accompany text rather than replace it.
- [ ] Use the existing palette as tokens: window `RGB(18,19,21)`, control surface `RGB(27,28,31)`, inactive `RGB(47,49,53)`, hover `RGB(62,65,70)`, primary blue around `RGB(55,139,226)`, primary text `RGB(240,240,242)`, secondary text `RGB(160,164,172)`.
- [ ] Give disabled controls lower-contrast text/borders without hiding them; draw a visible focus rectangle for keyboard navigation.
- [ ] Remove `Re-hook` from the toolbar; leave its guarded action under Advanced. Consolidate the three debug buttons behind one compact `Debug view` control/menu.
- [ ] Verify optical alignment at 100%, 125%, 150%, and 200% DPI and commit:

```text
feat: polish the native control strip
```

## Task 4: Eliminate GDI flicker at the source

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/UiLayout.h`
- Modify: `src/UiLayout.cpp`
- Modify: `tests/PolicyTests.cpp`

- [ ] Add failing tests for a helper that returns zero dirty rectangles when hover action is unchanged and exactly the old/new rectangles when it changes.
- [ ] Confirm RED, then implement the helper and pass its tests.
- [ ] Replace direct-to-window painting with: `BeginPaint`, compatible memory DC, compatible bitmap sized to the paint rectangle/client, one complete UI render into memory, one `BitBlt`, restore/delete GDI objects, `EndPaint`.
- [ ] Keep `WM_ERASEBKGND` returning nonzero and never paint the D3D12 child surface from the GDI path.
- [ ] Store `ToolbarAction m_hoverAction`; on `WM_MOUSEMOVE`, invalidate only the old/new button rectangles if the action changes. Use `TrackMouseEvent` and clear/invalidate the final hover on `WM_MOUSELEAVE`.
- [ ] Do not invalidate the full control strip for cursor movement, time polling, or unchanged status values.
- [ ] Cache the formatted status row; invalidate its rectangle only when the displayed string changes.
- [ ] Keep the non-client title stable as `DLSS Video Player — <media title>` and remove per-frame dimensions/FPS from `SetWindowTextW` calls.
- [ ] Use `GetGuiResources` before/after a 5-minute interaction loop to catch leaked GDI objects.
- [ ] Commit:

```text
fix: double buffer the native UI
```

## Task 5: Complete idle, status, error, and advanced flows

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/Localization.h`

- [ ] Make the empty surface expose two clear actions: `Open file` and `Open YouTube URL`; the second may be disabled until the YouTube plan lands, but its disabled reason must be explicit during intermediate commits.
- [ ] Add a single concise status row containing runtime mode, source/input/output dimensions, quality, rendered/source FPS, and dropped frames; truncate gracefully at narrow widths.
- [ ] Use precise states: `Neural addon enabled (experimental)`, `DLSS SR safe mode`, `Scaler fallback`, `Resolving YouTube…`, and short actionable errors.
- [ ] Add confirmation before Advanced `Re-hook` explaining that renderer recreation can reset or hang an experimental runtime.
- [ ] Ensure all toolbar actions have keyboard shortcuts or menu equivalents and that menus expose accelerator text.
- [ ] Test long media titles, no media, missing FFmpeg, unavailable DLSS, safe mode, and small window states.
- [ ] Commit:

```text
feat: clarify player states and recovery
```

## Task 6: Visual and performance verification

**Files:**
- Modify if needed: `docs/TROUBLESHOOTING.md`

- [ ] Run CTest and a clean Release build.
- [ ] On the RTX 4080, exercise idle, resize, maximize/restore, minimize/restore, rapid mouse sweeps, repeated menus, seek drag, volume drag, fullscreen entry/exit, and lower-output guarded re-hook.
- [ ] Capture before/after screenshots at idle, playing, safe mode, and error states; inspect rather than relying only on tests.
- [ ] Record a short screen capture during rapid hover/resize and confirm no visible control-strip flashes or black erases.
- [ ] Confirm GDI object count returns to its baseline range after repeated window recreation and no device-lost regression occurs.
- [ ] Run the Impeccable manual detector exactly once over the changed UI targets, as requested by its session context:

```powershell
node C:\Users\User\.agents\skills\impeccable\scripts\detect.mjs --json src\main.cpp src\UiLayout.cpp src\UiResources.cpp
```

- [ ] Fix real findings, document only narrow intentional exceptions, and rerun app/test verification after fixes.
- [ ] Commit any verification-driven corrections:

```text
fix: finish native UI polish
```
