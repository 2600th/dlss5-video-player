# DLSS Video Player release UI, RTX policy, YouTube, and packaging design

**Date:** 2026-08-31
**Status:** Approved

## Objective

Produce a simple, shareable Windows release of DLSS Video Player that:

- has a clearer, icon-assisted interface without visible control-bar flicker;
- contains the supplied experimental `nvngx_dlssnr.dll` for RTX 40 and RTX 50 users;
- enables the supplied experimental RenoDX neural-rendering path by default on RTX 40 and RTX 50 systems while retaining an explicit signed-DLSS safe mode;
- plays local files and most public, non-DRM YouTube videos;
- includes a short list of current official game and anime example videos;
- is English-only; and
- is distributed as one ZIP with one user-facing `DLSSVideoPlayer.exe` plus required helper binaries and DLLs.

This remains an experimental community build. It must not claim NVIDIA endorsement, universal RTX compatibility, or stability of the unsigned patched runtime.

## Non-goals

- A new UI framework, installer, updater service, account system, browser automation, or embedded web browser.
- Downloading or permanently caching YouTube videos.
- YouTube login/cookie extraction, DRM bypass, paid/private video access, or guaranteed support for live, age-restricted, or region-blocked content.
- Claiming hardware-verified RTX 50 behavior without an RTX 50 test machine.
- A custom ReShade loader, DXGI proxy, NGX hook, or modification of third-party binaries.
- Public publishing, code signing, or redistribution-rights approval. The output is a local package; the supplied patched DLL remains unsigned and its redistribution terms are not established by this project.

## Selected approach

Keep the current native Win32/D3D12 architecture and make targeted additions:

1. Double-buffer the existing GDI interface and reduce unnecessary invalidation.
2. Embed a pinned Tabler icon font and application icon as Windows resources.
3. Add small pure-policy modules for GPU classification, ReShade add-on state, and YouTube URL validation/resolution.
4. Use ReShade's supported `[ADDON] DisabledAddons` setting for explicit safe mode and unsupported GPUs rather than modifying ReShade or RenoDX.
5. Resolve YouTube URLs with bundled official `yt-dlp.exe` and Deno, then pass a single combined audio/video URL to the existing FFmpeg paths.
6. Extend the existing release script to assemble and verify one versioned ZIP.

A separate launcher was rejected because the user requested one user-facing EXE. Custom proxy/hook work was rejected as unnecessary and risky.

## UI design

### Layout

The video remains the dominant surface. The bottom control area is reorganized into three visual groups without changing the overall window architecture:

- **Playback:** Open, back 10 seconds, play/pause, stop, forward 10 seconds, mute.
- **Enhancement:** DLSS state, aspect mode, image adjustments.
- **View:** final image and compact debug-view controls, plus fullscreen.

`Re-hook` is removed from the primary toolbar. It remains under an **Advanced** menu and requires a confirmation explaining that renderer recreation can reset or hang an experimental driver/runtime combination. The same menu exposes **Restart in DLSS SR safe mode**, which disables only the RenoDX neural add-on for that launch.

The window title remains stable (`DLSS Video Player` plus the loaded media title) instead of being rewritten with per-frame diagnostics. A concise status row shows:

- detected GPU/runtime mode;
- source, DLSS input, and output dimensions;
- quality mode;
- rendered/source FPS and dropped frames; and
- clear states such as `Neural addon enabled (experimental)`, `DLSS SR safe mode`, `scaler fallback`, `resolving YouTube`, or an actionable error.

The idle view offers two primary actions: **Open file** and **Open YouTube URL**. An **Examples** menu provides curated links.

### Icons and accessibility

Download a pinned version of Tabler Icons from its official repository. Embed only the font/resource material required for this release and include the MIT license notice. Use familiar icons alongside short text labels; do not make controls icon-only. Keyboard shortcuts remain available.

The app receives a real application/window icon. Button hit targets remain at least 36 logical pixels high, colors retain strong contrast, active/disabled states are distinct, and text continues to use Segoe UI.

### Flicker fix

The root cause in the current control surface is direct-to-window GDI painting combined with whole-control-bar invalidation on every mouse move and frequent title/status changes.

The fix is deliberately small:

- paint the invalid region into a compatible memory DC/bitmap and perform one `BitBlt` to the window;
- keep `WM_ERASEBKGND` suppressed;
- invalidate only the old/new hover buttons when the hovered target changes;
- repaint status text only when its displayed values change;
- stop putting rapidly changing counters in the non-client window title; and
- keep the D3D12 render child separate from GDI UI painting.

No composition framework or retained-mode scene graph is introduced.

## RTX runtime policy

### Classification

Before renderer creation, enumerate the high-performance DXGI adapter and classify its description with pure, testable rules:

- names containing `GeForce RTX 40` are `Rtx40Ada`;
- names containing `GeForce RTX 50` are `Rtx50Blackwell`;
- other NVIDIA adapters are `OtherNvidia`;
- non-NVIDIA/no suitable adapter is `Unsupported`.

This intentionally targets most consumer RTX 40/50 systems without maintaining a fragile PCI-device table. Laptop names still match the generation prefix. Unknown cards fail safely.

### ReShade/RenoDX policy

The packaged `ReShade.ini` leaves the `renodx-dlss5.addon64` add-on enabled by default. ReShade 6.8 identifies this external add-on with canonical disabled-list token `DLSS 5 Neural Rendering@renodx-dlss5.addon64`; bootstrap configuration uses exact-case ReShade section/key semantics and migrates older aliases to that token. A current community installer reports that its RenoDX-author custom 310.8 runtime adds RTX 40 support while retaining RTX 50 support. The exact supplied file is version 310.8.0.0 with SHA-256 `28BDC080D28686DECDB63F6F4246B022274916B80AAFDAB266FE0FB63B2B9265`; this project has exercised that file only on an RTX 4080. The RTX 50 target is therefore a best-effort community compatibility goal, not an NVIDIA support statement or a local hardware verification.

Compatibility evidence recorded for this decision: <https://github.com/rakanki911/DLSS5-Swapper/releases/tag/v1.1.1>. That release does not publish a hash proving its embedded DLL is byte-identical to the supplied file, so the package must retain the limitation above.

At process start, before D3D12/NGX initialization:

- **RTX 40 and RTX 50:** ensure the add-on registered as `DLSS 5 Neural Rendering` from `renodx-dlss5.addon64` is enabled unless this launch explicitly requested safe mode.
- **Other NVIDIA and unsupported:** ensure that add-on is disabled with canonical token `DLSS 5 Neural Rendering@renodx-dlss5.addon64` while preserving normal DLSS SR/fallback behavior.
- **`--safe-mode`:** ensure that add-on is disabled with the same canonical token for the relaunched session, regardless of GPU generation.

If the required value changes, write the INI and relaunch the same EXE once with an internal bootstrap marker. The first process exits before renderer creation. A matching value proceeds without a restart. Loop prevention is based on the resulting configuration, requested mode, and marker. A later normal launch on RTX 40/50 restores the default neural-enabled policy.

If the INI cannot be updated or relaunch fails, display a clear message and exit before renderer creation rather than running in an unknown add-on state. The patched `nvngx_dlssnr.dll` remains in the folder in both modes but is not used while the RenoDX add-on is disabled. Signed `nvngx_dlss.dll` remains available for normal DLSS Super Resolution.

The status row reports configuration rather than inventing neural success: `Neural addon enabled (experimental)` or `DLSS SR safe mode`. Native NGX SR creation/evaluation remains separately visible. The UI directs users to the ReShade add-on panel for its own `RUNNING`/failure status. RTX 50 remains an intended best-effort target backed by community compatibility claims and policy tests; this development machine cannot hardware-verify RTX 50 execution.

## YouTube playback

### User flow

`File > Open YouTube URL...` and the idle-screen action open a small modal with:

- one URL text field;
- Paste, Play, and Cancel actions; and
- a short note: public, non-DRM videos only.

After Play, URL resolution runs on a worker thread. The main window remains responsive and shows `Resolving YouTube...`. Closing the app or opening another source cancels the resolver process.

### Validation and resolution

Accept only HTTPS URLs whose normalized host is YouTube (`youtube.com` subdomains or `youtu.be`). Reject quotes, control characters, excessive length, playlists without a selected video, and arbitrary local/network schemes before starting a helper process.

Use bundled official `yt-dlp.exe` with bundled Deno and bundled EJS scripts. Invoke it directly with `CreateProcessW`, `--no-config`, `--no-playlist`, no shell, and a bounded timeout. Select the best single combined audio/video format, preferring MP4, so the current decoder and audio player can consume the same direct URL:

```text
-f "b[ext=mp4]/b" --get-url
```

This intentionally favors simplicity and reliable A/V synchronization over separate high-resolution video/audio streams. Combined YouTube formats are commonly limited to lower resolutions, which is appropriate for demonstrating DLSS upscaling.

The resolved direct URL is ephemeral and exists only in memory. It is passed to the existing FFmpeg video and audio paths. It is never written to a playlist, download, or cache file.

### Supported and unsupported cases

Expected: most public, non-DRM, on-demand YouTube videos reachable without authentication.

Not guaranteed: private, members-only, paid, DRM-protected, age-restricted, region-blocked, login-required, playlist-only, and some live streams. Errors must identify whether validation, resolver startup, extraction, timeout, or FFmpeg opening failed.

### Examples

Add an `Examples` submenu with exactly six links: three games and three anime videos. Select current videos during implementation using these rules:

- verified official publisher/studio/channel;
- ordinary public watch URL, not a playlist or reupload;
- no age/login gate in a clean resolver smoke test;
- a useful mix of animation, motion, fine detail, and scene changes for upscaling evaluation; and
- title/channel/URL documented in the README with a note that availability can change.

Do not scrape trending lists at runtime. The curated table is compiled into the app for predictable behavior.

## English-only localization

- Delete `languages/pt-BR.lang`.
- Remove Portuguese aliases/default strings and the Language menu.
- Do not package the `languages` directory.
- Keep the current built-in English string lookup so UI text remains centralized without a broad rewrite.
- If an old INI contains `Language=pt-BR`, ignore it and run in English; no migration prompt is needed.
- Update README, localization documentation, build scripts, and CI package paths.

## Code organization

Keep additions small and testable:

- `src/RuntimePolicy.h/.cpp`: GPU generation classification and desired add-on state.
- `src/ReShadeConfig.h/.cpp`: parse/update only `DisabledAddons`, preserving unrelated settings.
- `src/YouTubeResolver.h/.cpp`: URL validation, helper discovery, bounded process execution, cancellation, and output parsing.
- `src/UiResources.h/.cpp` plus `.rc`: embedded icon font/application icon lifetime and lookup.
- `src/main.cpp`: presentation, dialogs, actions, bootstrap orchestration, and integration only.
- `tests/PolicyTests.cpp`: focused pure behavior tests without a large framework.

Avoid introducing a JSON library, networking library, dependency manager, UI toolkit, or service.

## Error handling and safety

- Enable the experimental add-on by default only on classified RTX 40/50 systems; provide `--safe-mode` and an Advanced-menu restart action.
- Preserve unrelated ReShade INI values and disabled add-ons.
- Never invoke a command shell for URLs.
- Bound URL length, helper runtime, and captured output.
- Terminate only helper processes created by this app.
- Do not log ephemeral resolved media URLs because they may contain tokens.
- Show concise user errors while retaining technical detail in `DLSSVideoPlayer.log`.
- Keep the signed rollback DLSS runtime outside the final distribution build inputs; the release manifest identifies which hashes were packaged.
- Run Microsoft Defender against the assembled stage and ZIP. Report unsigned/invalid signatures separately from malware scan results.

## Packaging

Produce one versioned directory and ZIP containing:

```text
DLSSVideoPlayer.exe          user-facing executable
ffmpeg.exe
ffprobe.exe
yt-dlp.exe
deno.exe
dxgi.dll                    ReShade add-on build
ReShade.ini                 RenoDX enabled by default for RTX 40/50 policy
ReShadePreset.ini
renodx-dlss5.addon64
nvngx_dlss.dll              signed DLSS Super Resolution runtime
nvngx_dlssnr.dll            supplied patched Ada runtime
sl.*.dll                    only the matching files proven required
docs/
README.md
LICENSE
THIRD_PARTY.md
THIRD_PARTY_LICENSES/
EXPERIMENTAL_RUNTIME_NOTICE.txt
PACKAGE_MANIFEST.txt        version, size, SHA-256, signature status
```

Only `DLSSVideoPlayer.exe` is presented as the launch target. Helper executables are implementation dependencies.

The packaging script must fail if required files are missing, Portuguese files are present, hashes do not match selected inputs, or the stage contains developer logs/INI state, test media, downloads, or rollback files.

## Verification and release gate

### Automated

- Configure and compile Release with MSVC `/W4` and no new warnings.
- Run CTest policy tests covering:
  - RTX 40/50/laptop/unknown/non-NVIDIA classification;
  - add-on enable/disable list preservation and idempotence;
  - default enablement on RTX 40/50, safe-mode override, and bootstrap-loop prevention;
  - accepted/rejected YouTube URL forms;
  - resolver-output parsing and limits;
  - Portuguese removal/package exclusion; and
  - required/forbidden package contents.
- Run the existing diagnostic scripts against packaged NVIDIA binaries.
- Verify manifest hashes against the staged files.

### Dynamic on this RTX 4080 system

- Idle window, resize/minimize/restore, rapid mouse movement, menu use, and fullscreen transitions with no visible control-strip flicker.
- Local video open, play/pause/seek/stop/volume/aspect/debug/fullscreen.
- DLSS OFF then ON at the safe 2560x1440 output.
- RTX 40 default launch leaves RenoDX enabled and evaluates successfully; a simulated stale disabled configuration is corrected with at most one relaunch.
- Simulated RTX 50 policy enables RenoDX by default; `--safe-mode` disables it idempotently and relaunches at most once.
- Patched `nvngx_dlssnr.dll` is loaded in the RTX 40 neural-enabled path and feature evaluation is observed; RTX 50 loading remains a simulated-policy check until tested on matching hardware.
- At least two curated YouTube examples resolve and play with synchronized audio.
- Invalid/private/unavailable URL produces a responsive, actionable failure.
- No new `nvlddmkm`/Display driver-reset events during the smoke window.

Do not repeat the known-unsafe 4K re-hook stress test as a release requirement. The Advanced re-hook path receives a warning and a targeted lower-output smoke test only.

### Audit

Review the changed code and package worst-first for:

- command/argument injection and unsafe URL handling;
- unbounded waits or process leaks;
- thread lifetime/use-after-free around resolver cancellation;
- destructive or lossy INI updates;
- unsafe add-on enablement on unknown/non-RTX GPUs and failure of the RTX 40/50 safe-mode escape path;
- device-lost handling and renderer recreation;
- redistribution notices, signatures, and unexpected binaries; and
- regression in local playback and fallback scaling.

The release may be called ready only when no confirmed critical/high-severity issue remains in the reviewed scope. Medium/low limitations and untested RTX 50 hardware behavior must be listed explicitly.

## Acceptance criteria

1. One ZIP is produced and opens to one obvious user-facing EXE.
2. The supplied Ada DLL is present with SHA-256 `28BDC080D28686DECDB63F6F4246B022274916B80AAFDAB266FE0FB63B2B9265` and clearly marked experimental/unsigned.
3. RTX 40 and RTX 50 policies enable RenoDX by default; unknown/non-RTX policy disables it before renderer initialization.
4. `--safe-mode` and the Advanced-menu action disable RenoDX and retain normal signed DLSS SR without a restart loop.
5. The control strip does not visibly flicker during the defined UI stress checks.
6. Icons are visibly used, embedded, pinned, and licensed.
7. Portuguese language files/menu/aliases are absent.
8. A public YouTube URL can be pasted and streamed without saving the video.
9. Curated official game/anime examples are present and at least two pass a clean smoke test at packaging time.
10. Automated tests/build/package checks pass and the audit reports no confirmed critical/high-severity issue.
11. Hardware and content limitations are stated honestly in the README and package notice.
