# Shareable Release Packaging and Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce and independently verify one shareable Windows x64 ZIP whose only obvious launch target is `DLSSVideoPlayer.exe`, with the exact patched neural DLL, matching runtime files, YouTube helpers, notices, and no confirmed critical/high issue in scope.

**Architecture:** Keep third-party binaries outside Git, pin every allowed release input by size/hash in a committed lock file, assemble a clean stage from an allowlist, generate a manifest, validate required/forbidden contents, and run static/dynamic/security release gates before declaring the ZIP ready.

**Tech Stack:** CMake/MSVC, PowerShell 7/Windows PowerShell, Authenticode, SHA-256, Microsoft Defender, ZIP/CTest.

**Spec:** `docs/superpowers/specs/2026-08-31-release-ui-youtube-design.md`

## Global Constraints

- Execute after all three implementation plans in the same isolated worktree.
- Use `superpowers:requesting-code-review` for the final worst-first review and `superpowers:verification-before-completion` before any ready/passing claim.
- Do not publish, upload, sign, or deploy the package without separate user authorization.
- Do not claim NVIDIA endorsement or verified RTX 50 execution.
- Do not include `downloads/`, test media, logs, personal INI state, build intermediates, source archives, or the signed rollback neural DLL.
- Never overwrite the supplied source binaries; copy only after exact hash verification.
- Packaging fails closed on missing/unexpected files, hash drift, Portuguese artifacts, or manifest mismatch.

---

## Task 1: Lock the exact third-party runtime inputs

**Files:**
- Create: `packaging/runtime-lock.json`
- Create: `packaging/ReShade.ini`
- Create: `packaging/ReShadePreset.ini`
- Create: `packaging/EXPERIMENTAL_RUNTIME_NOTICE.txt`
- Create: `tools/stage_runtime.ps1`
- Modify: `.gitignore`
- Modify: `THIRD_PARTY.md`

- [ ] Define lock entries with logical destination, exact byte size, SHA-256, expected signature state, provenance, and license/notice pointer.
- [ ] Pin the supplied patched DLL as:

```text
source name: nvngx_dlssnr_ada_v51_async.dll
destination: nvngx_dlssnr.dll
size: 165840496
file version: 310.8.0.0
SHA-256: 28BDC080D28686DECDB63F6F4246B022274916B80AAFDAB266FE0FB63B2B9265
Authenticode: HashMismatch (modified/invalid signature)
```

- [ ] Pin the matching supplied, validly NVIDIA-signed DLSS SR runtime instead of mixing the 310.8 neural runtime with the SDK checkout's current 310.7 file:

```text
source/destination: nvngx_dlss.dll
size: 58956400
file version: 310.8.0.0
SHA-256: C85F971CE023C9F3492FC7455F0B01A24BA18EA39636407A846902C4360B0B7E
Authenticode: Valid, NVIDIA Corporation
```

- [ ] Pin the locally verified ReShade/RenoDX inputs:

```text
dxgi.dll | 5592064 | 0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7 | NotSigned
renodx-dlss5.addon64 | 391168 | 87AEF9DDD937C7241E6BF8D8EFEA0045D63559135E254C60DAB316DB3D3A4AEE | NotSigned
```

- [ ] Seed the Streamline candidate lock with the matching pack:

```text
sl.common.dll | 830592 | A4B2B5ACBE49FBC6D44DD432CAC19CD53218F698B2539DC7ED0FB268C72CFC8D
sl.dlss.dll | 421504 | 1EB5FB3D6F01D340FE086D981CC2DE4F18AA6D05EE276E5CF28ECD54818DCC8B
sl.dlss_g.dll | 625792 | B8B5EFFD7DEBDB750ABD216DE43385FB653261712BC315D85EBA68811FB3EE02
sl.dlss_nr.dll | 401024 | 9F6672E5E0170DC118A3188D21BDA187E1FC1AA3502895B21AB846D23165C11D
sl.interposer.dll | 651392 | 27B2190057994C0B287C2C5716953BF1586F6499AC12FBBB2092B9AAF8396570
sl.nis.dll | 1155200 | 6039E38A1AF56C8E86F3E936596E2DB910BF3D76BBF4268562A3B13763049DFA
sl.pcl.dll | 360064 | 12AA4E76C28A27C735E4ECB3072F44D09428ACB107B70AC38E4BD48DDB05F88D
sl.reflex.dll | 382080 | ECF12973CDCEC2FFCED2EA77B1C7E45F4D387E7C864DDB5531B66A6F947EFFB3
```

- [ ] During the RTX 4080 neural smoke test, record loaded modules and retain only the matching Streamline candidates used by this configuration or explicitly required by the add-on's official pack; update lock and notice together if the set narrows.
- [ ] Add `stage_runtime.ps1 -InputDirectory <path> -Destination external/runtime` to match files by destination/source name, verify size/hash, report signature status, and copy only locked files. No user-specific absolute path may be committed.
- [ ] Ensure `external/runtime/` is ignored. Run staging against the supplied Downloads/MediaFire material and inspect every result.
- [ ] Author a clean `ReShade.ini` with `renodx-dlss5.addon64` not listed in `[ADDON] DisabledAddons`; do not copy the user's generated build INI/log.
- [ ] Explain experimental/unsigned status, RTX 40 local verification, RTX 50 best-effort target, safe mode, and redistribution uncertainty in the package notice.
- [ ] Commit:

```text
build: lock experimental runtime inputs
```

## Task 2: Make build inputs deterministic and bump the release

**Files:**
- Modify: `VERSION`
- Modify: `CMakeLists.txt`
- Modify: `build_windows.bat`
- Modify: `.github/workflows/build.yml`
- Modify: `.github/workflows/release.yml`
- Modify: `CHANGELOG.md`

- [ ] Bump the project and package version from `0.11.0` to `0.12.0` in one commit.
- [ ] Make `build_windows.bat` call the pinned UI/helper fetch scripts and validate the locked runtime directory before configure/build; remove opportunistic scanning of `.`, `streamline`, and `Streamline`.
- [ ] Stop arbitrary user files from overriding `nvngx_dlss.dll` during build.
- [ ] Copy only deterministic app dependencies into `build\Release`; keep release packaging as a separate explicit step.
- [ ] Update workflows to use the same pinned helper/runtime contract and English-only paths; CI without the private experimental runtime must fail the release job clearly while ordinary source compilation/tests can still run.
- [ ] Document 0.12.0 features and experimental limitations in `CHANGELOG.md`.
- [ ] Run a clean configure/build/test twice and compare hashes of deterministic source-built artifacts where toolchain timestamps permit; explain unavoidable PE timestamp differences.
- [ ] Commit:

```text
build: prepare version 0.12.0 release
```

## Task 3: Replace permissive packaging with an allowlisted assembler

**Files:**
- Create: `tools/package_release.ps1`
- Create: `tools/verify_package.ps1`
- Modify: `package_release.bat`
- Modify: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Add failing C++ tests for a pure required/forbidden filename policy, including rejection of `pt-BR.lang`, `languages/`, `downloads/`, `*.log`, developer settings, test media, PDB/object files, source archives, and rollback DLL names.
- [ ] Confirm RED, implement the list policy, and pass CTest.
- [ ] Make `package_release.bat` a thin checked wrapper around `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1`.
- [ ] Have the PowerShell assembler create a new `dist\DLSSVideoPlayer-v0.12.0-win64` stage and copy only an explicit allowlist:

```text
DLSSVideoPlayer.exe
ffmpeg.exe
ffprobe.exe
yt-dlp.exe
deno.exe
dxgi.dll
ReShade.ini
ReShadePreset.ini
renodx-dlss5.addon64
nvngx_dlss.dll
nvngx_dlssnr.dll
locked sl.*.dll files
README.md
LICENSE
THIRD_PARTY.md
THIRD_PARTY_LICENSES/**
docs/ARCHITECTURE.md
docs/BUILDING.md
docs/DLSS5_SETUP.md
docs/TROUBLESHOOTING.md
EXPERIMENTAL_RUNTIME_NOTICE.txt
PACKAGE_MANIFEST.txt
```

- [ ] Copy the exact locked, validly NVIDIA-signed 310.8 `nvngx_dlss.dll` from `external/runtime`; do not silently substitute the SDK checkout's 310.7 file or an arbitrary local build output.
- [ ] Verify yt-dlp/Deno against their pinned hashes and all experimental files against `runtime-lock.json` before copying.
- [ ] Generate `PACKAGE_MANIFEST.txt` in stable filename order with product version, file path, byte size, SHA-256, and Authenticode status/signer for PE files.
- [ ] Make verifier reject any stage file absent from the allowlist, any required file missing, any Portuguese path/content marker, any hash mismatch, empty ReShade preset/config, or unexpected launchable `.exe` outside the four known helpers/app.
- [ ] Compress to `dist\DLSSVideoPlayer-v0.12.0-win64.zip`, re-open the ZIP, enumerate it, and rerun manifest/content validation on extracted temporary contents.
- [ ] Commit:

```text
build: assemble allowlisted Windows release
```

## Task 4: Update user-facing release documentation

**Files:**
- Modify: `README.md`
- Modify: `docs/DLSS5_SETUP.md`
- Modify: `docs/BUILDING.md`
- Modify: `docs/TROUBLESHOOTING.md`
- Modify: `SECURITY.md`
- Modify: `THIRD_PARTY.md`

- [ ] Lead README with: extract the whole ZIP, run `DLSSVideoPlayer.exe`, open a local file or YouTube URL, and use Advanced safe mode if the experimental neural path misbehaves.
- [ ] State exactly: tested locally on RTX 4080; RTX 40 and RTX 50 are intended policy targets; RTX 50 was not hardware-tested here; patched neural runtime is modified and has invalid Authenticode status.
- [ ] Describe normal/default neural mode versus `--safe-mode` without claiming that configuration state proves a successful neural evaluation.
- [ ] Document public non-DRM YouTube limitations and the six examples, with availability-change caveat.
- [ ] Remove obsolete instructions that ask users to assemble or run helper BAT files; only the EXE should be presented as the launch target.
- [ ] Add upstream source/license notices for ReShade, RenoDX, NVIDIA runtimes/Streamline, FFmpeg, yt-dlp, Deno, and Tabler; flag unresolved redistribution permission prominently rather than inventing permission.
- [ ] Commit:

```text
docs: write the 0.12.0 release guide
```

## Task 5: Run functional and compatibility gates on RTX 4080

**Files:**
- Create: `docs/release/0.12.0-verification.md`

- [ ] Start from a freshly extracted ZIP in a path containing spaces and non-ASCII characters.
- [ ] Verify only `DLSSVideoPlayer.exe` is needed as the user launch action.
- [ ] Exercise local playback: open, play/pause, ±10 seek, timeline seek, stop, volume/mute, aspect, adjustments, debug view, fullscreen, DLSS off/on at safe 2560x1440 output.
- [ ] Exercise normal RTX 4080 launch, stale-disabled INI correction, safe-mode launch, and later normal-mode restoration; verify at most one bootstrap relaunch each time.
- [ ] Confirm through loaded-module evidence and the ReShade add-on panel that the patched `nvngx_dlssnr.dll` is loaded/evaluated in default mode. Record exact hash and observed status without extrapolating to RTX 50.
- [ ] Exercise one game and one anime YouTube example for synchronized playback, then invalid/private/cancelled/offline cases.
- [ ] Run UI resize/hover/menu/fullscreen stress and confirm no visible toolbar flicker or GDI leak.
- [ ] Inspect Windows System event log for new `nvlddmkm`, display-driver reset, WHEA, or application-crash events within the smoke window.
- [ ] Run simulated policy tests for RTX 50, other NVIDIA, AMD/Intel, and missing adapters.
- [ ] Record commands, exact automated test count, artifact hashes, observed behaviors, and limitations in the verification report.
- [ ] Do not run the unsafe 4K re-hook stress test.
- [ ] Commit:

```text
test: record 0.12.0 release verification
```

## Task 6: Run security, package, and worst-first code audit

**Files:**
- Create: `docs/release/0.12.0-audit.md`
- Modify as findings require: touched source/build/docs files only

- [ ] Run clean build/CTest/package verification and preserve outputs in the audit summary, not as generated logs committed to the package.
- [ ] Review URL flow source-to-sink for scheme/host confusion, argument injection, token logging, arbitrary helper execution, oversized output, and non-shell guarantees.
- [ ] Review resolver lifetime for unbounded waits, leaked handles/processes, stale completion messages, cancellation races, UI-thread blocking, and use-after-free.
- [ ] Review INI updates for truncation, unrelated-value loss, case/list errors, unsafe default enablement, and bootstrap loops.
- [ ] Review renderer integration for device-lost/re-hook regression and unknown-GPU fail-safe behavior.
- [ ] Review packaging for unexpected binaries, missing notices, hash drift, signature status, personal state, Portuguese artifacts, and rollback inclusion.
- [ ] Run Microsoft Defender custom scans against both the stage directory and final ZIP; record engine/signature versions, timestamp, and result. Report `HashMismatch`/`NotSigned` separately from malware detection.
- [ ] Use the required final code-review workflow and classify every confirmed finding by critical/high/medium/low. Fix critical/high findings and rerun all affected gates; do not call the release ready while any remains.
- [ ] List residual medium/low issues, external-video availability, redistribution uncertainty, and untested RTX 50 hardware explicitly.
- [ ] Commit fixes separately by concern, then commit the final audit record:

```text
docs: record 0.12.0 release audit
```

## Task 7: Final artifact verification and handoff

**Files:**
- No source changes expected

- [ ] Run from a clean terminal:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
package_release.bat
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\verify_package.ps1 -Zip dist\DLSSVideoPlayer-v0.12.0-win64.zip
git status --short
```

- [ ] Compare final ZIP SHA-256 with the recorded verification/audit document and `PACKAGE_MANIFEST.txt` contents.
- [ ] Confirm the ZIP is outside Git, no untracked generated files except preserved user `downloads/` and ignored release/runtime outputs, and no unrelated worktree change was modified.
- [ ] Report: absolute artifact path, ZIP size/hash, test counts, Defender result, RTX 4080 evidence, simulated RTX 50 result, confirmed critical/high count, residual limitations, commit range, and push state.
- [ ] Do not upload or publish the ZIP unless the user explicitly requests it.
