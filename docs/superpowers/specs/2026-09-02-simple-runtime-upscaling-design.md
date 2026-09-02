# Simple runtime DLSS upscaling

Date: 2026-09-02

Status: Approved by the user on 2026-09-02; implementation in progress.

## Goal

Add independent, optional DLSS Super Resolution (SR) to playback of either the
original video or the existing neural-rendered cache. Keep Neural Rendering on
by default, SR off by default, and Frame Generation off and unavailable.

This supersedes the earlier controls change's prohibition on adding SR
integration. Preserve its cache, seek, source-selection and button-padding fixes.

## Smallest dependable separation

The existing player loads the ReShade/RenoDX neural add-on into its own process.
Its raw NGX calls deliberately feed that add-on. Calling the same backend with
larger dimensions is not sufficient proof of independent SR.

Use one hidden neural-cache helper executable in a separate runtime directory.
Move the existing offline neural-rendering invocation into that helper; reuse
the existing renderer, decoder, encoder and neural-evidence validation code.
The main player must not load the neural add-on or its DXGI proxy. Its NGX
backend can then evaluate ordinary SR on the selected playback frames.

Communicate source/output paths and render settings to the helper using quoted
arguments. A small versioned progress/result channel carries only job metadata,
not video frames. Cancellation and parent exit must terminate the owned helper
and its encoder children. Only validated successful results can be promoted by
the main process's existing cache manager. Missing, malformed or failed helper
results leave the cache unpublished and preserve the original-video fallback.

Because rendering already finishes before cached playback begins, use files as
the media handoff. Do not introduce shared textures, cross-process GPU fences,
services, sockets, Python, Streamline, a new NR implementation, or a new optical
flow dependency. Keep existing runtime versions unchanged.

Alternatives considered:

- Calling the hooked backend directly is smaller but does not establish
  independence from NR; do not ship that as SR.
- Replacing NR with ComfyUI's direct native bridge could remove the hook, but
  changes the currently validated neural path and depends on undocumented NR
  details. Defer it.

## Playback behaviour

- Enable the existing DLSS Upscaling button and matching top-menu toggle only
  when a usable independent SR backend and a larger output size are available.
- Offer 1440p and 2160p output targets in the DLSS menu; default to 1440p.
  Preserve aspect ratio. Never reduce the source to meet a target or call a
  same-size operation "upscaling". Explain when the source already meets or
  exceeds the chosen target.
- Keep source-resolution selection separate from output resolution. Preserve
  the existing 1080p preference, highest-available fallback and lack of 480p/720p
  source-menu choices.
- Reuse the existing temporal guides for the first version. Choose a supported
  NGX input/output configuration that preserves the decoded source resolution;
  reject unsupported combinations clearly rather than silently downsampling.
- Evaluate SR only on the selected original or neural playback frame. Toggling
  SR, switching comparison view or changing output resolution must not modify
  cached media, trigger neural rendering or redownload the source.
- Preserve playback position and play/pause state on toggles. Reset temporal
  history on seeks, source/view changes and SR reconfiguration; do not reset it
  on every consecutive video frame.
- If SR initialization/evaluation fails, turn it off, display a concise reason
  and continue ordinary playback. A successful generic API call alone must not
  be reported as proof of image improvement.
- Keep the current padded controls and all existing cache/seek/EOF behaviour.

## Targeted changes

- A small neural-helper entry point and process wrapper around
  `OfflineNeuralRenderer`; reuse existing processing classes.
- Startup/runtime layout and packaging: hook-free player at the package root,
  neural add-on/runtime beside the helper in a dedicated subdirectory.
  Resolve FFmpeg and other shared helpers explicitly; avoid unnecessary copies.
- `DLSSBackend` / `D3D12Renderer`: independent playback SR configuration, actual
  source-to-output dimensions and safe feature/resource recreation.
- `main.cpp`, native menu and localization: independent SR preference,
  capability/error state, output target and synchronized menu/toolbar controls.
- Focused regression tests and documentation of actual supported behaviour.

Do not add FG support, new quality-preset menus, general backend abstractions,
fresh runtime downloads, cache-schema changes, tagging or publication.

## Verification gate

Write failing regressions before each implementation step. Cover helper
success/failure/cancellation, unpublished incomplete results, defaults and
capability state, source-preserving target selection, menu/toolbar agreement,
and continued cache reuse. Run the existing regression suites and Release build.

On this RTX 5090, test the Resident Evil example at 1080p input and 1440p output:
verify the playback process has no neural hook loaded, actual SR evaluations
produce the larger output, cached media remains unchanged, and toggles/seeking/
EOF/source replacement preserve playback. Measure dropped frames and compare
moving detail against ordinary scaling at the same display size. Test 2160p
as a selectable target without promising real-time performance in advance.

An RTX 5090 result does not validate RTX 40 hardware. Report that boundary and
any unrun checks explicitly. Do not scan antivirus or change security settings.
Preserve previous packages and unrelated worktree changes.

## Self-review

The only new process is the offline neural-cache helper. Playback SR stays in
the existing renderer, with one toggle and two output targets. Media does not
cross a new live IPC pipeline. Existing NR runtime, cache format and temporal
guide implementation remain unchanged; uncertain SR quality is a verification
gate rather than an assumed benefit.
