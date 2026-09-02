# Independent DLSS Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver three honest, synchronized DLSS controls and default cached playback to Neural Rendering without regressing cache or seek behaviour.

**Architecture:** Keep the existing offline neural renderer and synchronized playback. Separate the neural view preference from renderer SR state; feature availability drives both menu and toolbar. FG remains unavailable, and SR remains unavailable unless an existing independent runtime path is actually verified.

**Tech Stack:** C++20, Win32/GDI, D3D12/NGX, CMake/MSVC, CTest, PowerShell packaging.

**Spec:** `docs/superpowers/specs/2026-09-02-independent-dlss-controls-design.md`

## Global Constraints

- Neural Rendering starts requested on; DLSS Upscaling and Frame Generation start off.
- No Frame Generation backend, Streamline integration, worker-process split, runtime replacement, or undocumented hook manipulation.
- Unsupported functionality is disabled and explained, not presented as active.
- Preserve current cache identity, pre-render-before-play, synchronized original comparison, audio, pause, seek, and highest-available source fallback.
- Do not reintroduce 480p or 720p source choices.
- Preserve 10-DIP horizontal padding, 6-DIP vertical padding, and 7-DIP icon-label gap, with non-overlapping controls across supported widths and DPI.
- Preserve all existing uncommitted changes. No commit, tag, push, security-setting change, or antivirus scan.
- Implement in the current task checkout; dependencies and prior fixes are present here. Do not create another checkout or move files.

## Task 1: Separate playback features, menus and toolbar

**Files:**
- Modify `src/main.cpp`, `src/AppMenu.h`, `src/AppMenu.cpp`, `src/UiLayout.h`, `src/UiLayout.cpp`, `src/Localization.h`.
- Modify existing tests in `tests/PlayerUiRegressionTests.cpp` and `tests/PolicyTests.cpp`.
- Modify `README.md`, `CHANGELOG.md`, and `docs/ARCHITECTURE.md` for actual shipped behaviour.
- Only if needed, add a small shared header for feature presentation/availability; avoid a new processing subsystem.

**Interfaces:**
- Consumes `SynchronizedPlayback::SetView(ComparisonView)`, `PlayerApp::RenderVideoFrame`, `app_menu::CreateMenuBar`, `LayoutToolbar`, shared padded button layout, and the existing player test-access fixture.
- Produces distinct `ToolbarAction` values for Neural Rendering, Upscaling, and Frame Generation, plus corresponding distinct menu IDs. The existing neural keyboard/global-hotkey route continues to select the neural view only.
- One player-derived availability/state snapshot must feed both menu and toolbar; command handlers must independently enforce the same guards.

- [x] **Step 1: Extend the existing native-player test before implementation.**

  Use the existing hidden-window `PlayerAppTestAccess` fixture to prove the former generic toggle is not an ambiguous SR control. Assert consumer-visible labels, checked/disabled states, and forbidden dispatch outcomes, not source text. Start with an assertion against current callable behaviour, for example the existing toolbar content for the generic toggle must name Neural Rendering rather than DLSS. Run it and record the expected assertion failure before changing production code.

  ```cpp
  const auto content = app.ButtonContent(ToolbarAction::ToggleDlss);
  CHECK(content.label.find(L"Neural") != std::wstring::npos);
  ```

- [x] **Step 2: Implement the feature state and neural default.**

  Rename the ambiguous neural toolbar action/handler to an explicit neural name. Maintain a session preference initialized to true, distinct from actual available/active state. Use that preference for both `SetView` and the first cached frame, before presenting; do not flash Original first. Use the same requested view on the next clip. Preserve the existing last-presented synchronized-pair selection when toggled while paused or playing.

  ```cpp
  bool m_neuralRequested = true;
  const ComparisonView desiredView = m_neuralRequested
      ? ComparisonView::Neural : ComparisonView::Original;
  // Read the first synchronized pair, then select desiredView before presentation.
  // Interactive renderers call SetDLSS(false); offline carrier remains unchanged.
  ```

  Keep SR and FG unavailable for this delivery unless the existing safe-mode path can be verified without extra implementation. The known hooked cached path must never expose SR. Reject stale/keyboard/native calls to unavailable controls; disable legacy SR quality commands too. Normal interactive playback must not accidentally inherit the renderer's internal true default.

- [x] **Step 3: Add menu/toolbar identity and layout tests, then implement the UI.**

  Add the two new disabled feature controls and replace generic wording with explicit names. Synchronize menu state during all existing UI invalidation/load/unload paths. Put depth diagnostics in Advanced. Remove misleading ordinary DLAA/quality choices from the end-user DLSS menu when SR is unavailable; legacy command IDs may remain guarded for compatibility.

  Use the existing vector icon system and padded layout. Feature names and states must remain readable at the supported minimum width: reserve a dedicated feature row if needed rather than shrinking all three to ambiguous icons. Adjust toolbar height/hit-testing/minimum size together if using a second row.

  Test the real layout and GDI content bounds at 96, 120, 144, and 192 DPI. Check every button's non-overlap, usable hit size, label content padding, and visibility within the client rectangle. Test toolbar/menu parity at idle, loading, paused, playing, seeking, EOF and unload. Verify unavailable commands cannot change preferences or trigger reloads.

- [ ] **Step 4: Add/extend cached view regression coverage.**

  Verification boundary: existing synchronized-source tests cover frame selection, pause/seek/EOS; actual PlayerApp initial Neural, same-frame toggle, paused/playing seek, EOF reopen, and preference persistence were verified live with the packaged Resident Evil cache. No new GPU-backed PlayerApp automated integration fixture was added. Task and final reviewers accepted this boundary.

  Exercise cached initial view, off/on same-timestamp frames, preserved pause/play state, and preference across unload/load. Use existing fake-pipe media fixtures or hidden-window player tests where available; keep mocks at external Win32/media boundaries only. Do not change cache schema or source-selection logic.

- [x] **Step 5: Run focused tests, then full Release verification.**

  ```powershell
  & 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' --build build --config Release --parallel
  & 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir build -C Release --output-on-failure
  git diff --check
  ```

  Report which relevant assertions failed before implementation and passed afterward. Do not commit. Controller owns live UI/hardware checks, repeat final suites, packaging, and review.

- [x] **Step 6: Update documentation and self-review.**

  Describe NR on/default and synchronized cached comparison. Explicitly state FG is unavailable; state SR's actual result (expected unavailable in the hooked cached path), without promising a capability merely because DLLs exist. Preserve unrelated documentation and release notices. Report changed files, commands/results, limitations, and concerns in the task report.

## Controller delivery checks

- [x] Review the task's delta against the pre-task snapshot, preserving unrelated dirty changes.
- [x] Run the full CTest suite twice on final sources.
- [x] Inspect live menu/toolbar once, with at most one corrective visual pass; verify Resident Evil cache replay, NR comparison, seek/EOF/reload and log/cache evidence.
- [x] Run the broader final review and resolve material findings.
- [x] Back up the exact previous private package, rebuild/package, and verify archive contents and hash; no tag/push.
- [x] Report the build location and actual unavailable features, distinguishing test coverage from hardware checks.
