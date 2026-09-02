# Independent DLSS controls: reduced-scope design

Date: 2026-09-02

Status: Written design approved by the user on 2026-09-02.

## Objective

Replace the ambiguous single DLSS toggle with distinct Neural Rendering, DLSS Upscaling, and Frame Generation controls. The bottom toolbar and top DLSS menu must show the same requested state, actual availability, and active state.

The user explicitly excluded functionality that would require a new integration beyond the existing bundled runtime and backend. Do not implement a Frame Generation backend, introduce Streamline, or split rendering into a new worker process for this change.

## Behaviour

| Feature | Initial preference | Supported behaviour in this change |
| --- | --- | --- |
| Neural Rendering | On | Use the existing pre-render cache; toggle between synchronized neural and original frames. |
| DLSS Upscaling | Off | Runtime only, and available only in an existing playback path verified to perform independent SR. Otherwise disabled with an explanation. |
| Frame Generation | Off | Disabled; display "Unavailable in this build". No generated-frame processing is added. |

An initial preference is not proof of availability. If neural rendering cannot run, explain the unavailable runtime and do not claim the displayed output is neural rendered. User selection during an app session must survive opening another clip; a fresh session starts with Neural Rendering requested and SR/FG off.

### Neural Rendering

- New cached playback starts on the neural frame when the neural preference is on, replacing the current hard-coded Original view.
- Turning the control off shows the corresponding original frame at the same playback position. Turning it back on restores the neural frame.
- Preserve play/pause state, audio position, pending synchronized frames, and seek state when switching views.
- Switching a completed cached video must not download, re-encode, or invalidate its cache.
- Preserve the current pre-render-before-play workflow and cache validation. This change does not add a separate source-only loading pipeline or live neural rendering.
- In a playback path with no synchronized neural output, show why the comparison control is unavailable; do not repurpose it as an SR toggle.

### DLSS Upscaling

- Default off in every interactive playback path. The offline renderer's internal DLAA carrier is not a user-facing upscaling setting and must remain functional.
- Apply any enabled SR to the selected playback frames at runtime, never to the saved neural/original cache files.
- The current raw NGX SR entry points are also intercepted by the neural add-on. A successful generic NGX evaluation is therefore insufficient evidence of independent runtime upscaling.
- Keep SR disabled during cached neural playback unless the existing integration can be demonstrated to bypass the neural hook safely. Do not add process isolation, a new rendering subsystem, or undocumented hook manipulation to enable it.
- The existing SR safe-mode path may expose SR only if it independently evaluates SR, produces genuinely higher-resolution output, and preserves playback on toggle. Do not label a 1:1 DLAA operation or ordinary display scaling as DLSS Upscaling.
- An unavailable feature remains off. Its explanation must distinguish missing implementation or hook conflict from unsupported GPU hardware.
- If no existing path satisfies these requirements, leave SR unavailable for this build. This is the approved fallback, not a reason to expand scope.

### Frame Generation

- Keep a distinct disabled toolbar button and matching disabled menu entry so the three features are not conflated.
- Never report FG active or generated frames without a backend.
- No additional FG downloads, SDK integration, swap-chain work, or system-settings changes.

## Interface

- Bottom toolbar: three separately labelled feature buttons, with visible on/off or unavailable state. Use the existing icon system and shared padded button layout.
- Preserve at least the existing 10-DIP horizontal inset, 6-DIP vertical inset, and 7-DIP icon-to-label gap. Test the added controls at the supported minimum window width and multiple DPI scales; avoid clipping or overlap.
- Top DLSS menu: matching Neural Rendering, DLSS Upscaling, and Frame Generation entries. Checkmarks and enabled states come from the same feature state as toolbar buttons.
- Remove the misleading generic "Enable DLSS" wording. SR quality options belong to SR and are unavailable when SR is unavailable. Do not present DLAA carrier quality as neural-rendering quality.
- Keep depth-proxy and re-hook diagnostics under Advanced rather than making them look like independent end-user DLSS features.
- Retain the existing D shortcut for Neural Rendering; unavailable commands must also be guarded in keyboard and native-command dispatch. Leave the existing G depth-debug shortcut unchanged.
- Status text distinguishes the selected original/neural view from runtime SR and unavailable FG.
- Source quality remains default/minimum requested 1080p, with existing higher-resolution choices and highest-available fallback. Do not reintroduce 480p or 720p menu options.

## Implementation boundaries

- `src/main.cpp`: independent feature preferences, cached initial view, command routing, actual-state updates and toolbar/status text.
- `src/AppMenu.h` and `src/AppMenu.cpp`: distinct commands and synchronized feature menu state; diagnostic placement.
- `src/UiLayout.h` and `src/UiLayout.cpp`: distinct toolbar actions, availability and padded layout.
- `src/Localization.h`: explicit feature names and unavailable reasons, using the existing UTF-8-safe build configuration.
- `src/D3D12Renderer.h` and related playback setup: change interactive SR defaults only where necessary; do not break the offline neural carrier.
- Existing regression tests: add feature-state/menu/layout coverage and cached-view transition coverage. Preserve previous cache and seek fixes.
- Documentation: describe the actual supported features, including unavailable FG and any SR limitation, without claiming runtime support from DLL presence.

No unrelated refactor, runtime replacement, cache-schema bump, commit, tag, push, or publication is included. Preserve existing uncommitted changes. Produce a local testable build after verification; do not overwrite or remove prior deliverables without resolving exact targets first.

## Verification and acceptance

1. Write regression tests before implementation and observe relevant failures.
2. Verify fresh preferences: NR requested on, SR off, FG off; actual state never claims unavailable processing is active.
3. Verify toolbar and native-menu states agree at startup, load completion, pause, seek, EOF, and source replacement. Disabled command dispatch must be a no-op.
4. Verify initial cached display is neural when requested; NR off/on selects synchronized original/neural frames without changing playback position or pause state.
5. Verify reopening and toggling completed videos preserves cache files and does not re-encode.
6. Verify clicking/dragging the seekbar, seeking after EOF, and selecting another example still work.
7. Verify button content bounds, minimum hit area, and no overlap at supported narrow widths and 100%, 125%, 150%, and 200% DPI.
8. If SR is enabled anywhere, verify independent runtime SR evaluations, input/output dimensions, no cache writes, and preserved playback on toggle. Otherwise verify its explicit unavailable reason.
9. Build Release, run all existing CTest suites twice, and run `git diff --check`. Report any unrun visual or hardware checks honestly. RTX 5090 testing cannot establish RTX 40-series hardware validation.
10. Update the private local package only after the build and applicable tests pass. Do not scan antivirus or change security settings.

## Evidence used for scope

- Local `src/main.cpp`: the current cached toggle changes `ComparisonView` and explicitly disables renderer DLSS; cached load currently selects Original.
- Local `src/DLSSBackend.cpp` and `src/ReShadeConfig.cpp`: SR uses raw NGX calls visible to the enabled neural hook, with `NeuralUplift=1` and `NREnableUpscaling=0` for pre-rendering.
- NVIDIA's [DLSS Frame Generation integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md): FG integration requires per-frame inputs/constants and presentation/lifetime handling beyond DLL presence. This supports excluding FG backend work from this change.

## Self-review

The design keeps the existing cached neural pipeline, makes unsupported controls honest, and adds no new runtime subsystem. Defaults apply to user-facing playback preferences, not the offline DLAA carrier. SR/FG never become cache-key inputs in this scope. The explicit unavailable fallback resolves the known neural-hook conflict without expanding the approved work.
