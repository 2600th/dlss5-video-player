# Runtime Policy and English-Only Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable the supplied RenoDX neural add-on by default on supported RTX 40/50 systems, provide a deterministic signed-DLSS safe mode, and remove Portuguese without broad architectural change.

**Architecture:** Add two small pure modules for GPU/runtime policy and ReShade INI mutation, then call a thin Windows bootstrap before renderer initialization. Keep English strings centralized in the existing localizer, but remove runtime language selection and external language files.

**Tech Stack:** C++20, Win32, DXGI 1.6, CMake/CTest, MSVC 2022, ReShade INI format.

**Spec:** `docs/superpowers/specs/2026-08-31-release-ui-youtube-design.md`

## Global Constraints

- Work from an isolated worktree created with `superpowers:using-git-worktrees`.
- Use `superpowers:test-driven-development`; read its `writing-good-tests.md` before the first production edit.
- Preserve the untracked `downloads/` tree and all unrelated user changes.
- Do not modify or regenerate third-party DLLs.
- Treat RTX 50 support as a tested policy target, not locally hardware-verified behavior.
- The packaged neural add-on is default-on only for classified RTX 40/50 adapters; unknown and unsupported GPUs fail safe.
- Run `superpowers:verification-before-completion` before any completion claim.

---

## Task 1: Add a framework-free native test target

**Files:**
- Create: `tests/TestSupport.h`
- Create: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Add `include(CTest)` and a `PolicyTests` console executable under `if(BUILD_TESTING)`.
- [ ] Give the test target the same C++20, Unicode, `/W4`, `/permissive-`, and `/EHsc` settings as the application.
- [ ] Link only the Windows libraries required by the modules under test (`dxgi`, `shlwapi`, `shell32`) and register `add_test(NAME PolicyTests COMMAND PolicyTests)`.
- [ ] Implement a tiny test runner with `CHECK`, `CHECK_EQ`, named test functions, a failure counter, and a nonzero exit code; do not add Catch2, GoogleTest, or a package manager.
- [ ] Add a single harness sanity test and run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --target PolicyTests
ctest --test-dir build -C Release --output-on-failure
```

- [ ] Confirm CTest reports exactly one passing test target.
- [ ] Commit:

```text
test: add native policy test harness
```

## Task 2: Implement GPU classification and neural policy

**Files:**
- Create: `src/RuntimePolicy.h`
- Create: `src/RuntimePolicy.cpp`
- Modify: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Write failing table tests for desktop and laptop descriptions, mixed case, other NVIDIA, AMD/Intel, and missing adapters.
- [ ] Cover this public API exactly:

```cpp
enum class GpuGeneration { Rtx40Ada, Rtx50Blackwell, OtherNvidia, Unsupported };

struct DetectedGpu {
    GpuGeneration generation{GpuGeneration::Unsupported};
    std::wstring description;
};

GpuGeneration ClassifyGpu(uint32_t vendorId, std::wstring_view description);
bool NeuralAddonDesired(GpuGeneration gpu, bool safeMode);
DetectedGpu DetectHighPerformanceGpu();
```

- [ ] Assert `NeuralAddonDesired` is true only for RTX 40/50 without safe mode.
- [ ] Build the test target and confirm the new tests fail because the module is absent.
- [ ] Implement case-insensitive prefix matching for `GeForce RTX 40` and `GeForce RTX 50` only when `vendorId == 0x10DE`.
- [ ] Implement adapter discovery with `CreateDXGIFactory2` and `EnumAdapterByGpuPreference(..., DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, ...)`; skip software adapters and return the first usable hardware adapter.
- [ ] Keep DXGI enumeration separate from pure classification so simulated RTX 50 tests do not need RTX 50 hardware.
- [ ] Add the module to both targets, rerun CTest, and confirm all cases pass.
- [ ] Commit:

```text
feat: classify RTX runtime policy
```

## Task 3: Preserve and update ReShade add-on state safely

**Files:**
- Create: `src/ReShadeConfig.h`
- Create: `src/ReShadeConfig.cpp`
- Modify: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Write failing tests for missing `[ADDON]`, empty and populated `DisabledAddons`, mixed line endings, unrelated sections, duplicate list entries, idempotence, and filenames with similar substrings.
- [ ] Define the pure and filesystem APIs:

```cpp
struct ConfigUpdate {
    bool ok{false};
    bool changed{false};
    bool addonEnabled{false};
    std::wstring error;
};

std::string UpdateDisabledAddonsIni(
    std::string_view ini,
    std::string_view addonName,
    bool disabled);

ConfigUpdate ConfigureNeuralAddon(
    const std::filesystem::path& iniPath,
    bool enable);
```

- [ ] Confirm the tests fail before production code exists.
- [ ] Model pinned ReShade 6.8 semantics: strip a leading UTF-8 BOM for lookup, match the exact-case `[ADDON]` section and `DisabledAddons` key, and preserve unrelated bytes and the target section's line-ending style.
- [ ] Treat `DisabledAddons` as a comma-separated set and manage canonical token `DLSS 5 Neural Rendering@renodx-dlss5.addon64`. Migrate/remove the exact registered-name and `@filename` forms plus legacy/wrong-case aliases, retain unrelated entries, and collapse managed duplicates.
- [ ] Write changed content to a sibling temporary file, flush/close it, then use `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`; leave the original untouched on failure.
- [ ] Return `changed == false` for already-correct content and verify the second update is byte-identical.
- [ ] Run CTest and inspect a fixture diff to prove unrelated INI settings remain unchanged.
- [ ] Commit:

```text
feat: manage RenoDX addon state safely
```

## Task 4: Add the pre-renderer bootstrap and one-relaunch guard

**Files:**
- Modify: `src/RuntimePolicy.h`
- Modify: `src/RuntimePolicy.cpp`
- Modify: `src/main.cpp`
- Modify: `tests/PolicyTests.cpp`

- [ ] Add failing tests for the action matrix using:

```cpp
enum class BootstrapAction { Continue, Relaunch, Fail };

BootstrapAction DecideBootstrap(
    bool desiredEnabled,
    bool configEnabled,
    bool alreadyRestarted,
    bool updateSucceeded);
```

- [ ] Cover: matching state continues; a successful mismatch relaunches only before the marker; a persistent mismatch after the marker fails; an update failure fails.
- [ ] Confirm RED with the test target.
- [ ] Parse only `--safe-mode` and internal `--addon-bootstrap-restarted` at startup; reject neither normal file arguments nor future options.
- [ ] Before creating the main window, D3D12 device, NGX, or ReShade-dependent renderer state: detect the GPU, compute desired state, and call `ConfigureNeuralAddon` on the EXE-directory `ReShade.ini`.
- [ ] If state changes, relaunch the same absolute executable with preserved user arguments plus the internal marker via `CreateProcessW`, with correctly quoted Windows arguments and no shell.
- [ ] On update or relaunch failure, show one concise `MessageBoxW`, log the technical error, and exit before renderer creation.
- [ ] Store the detected generation and requested mode for the later status row; label configured state as experimental, not as proof the neural feature evaluated.
- [ ] Add `Advanced > Restart in DLSS SR safe mode`; confirm once, relaunch the same EXE with `--safe-mode`, then close the current process.
- [ ] Run policy tests and launch twice against temporary copied INI fixtures to verify at most one relaunch per correction.
- [ ] Commit:

```text
feat: bootstrap neural addon policy
```

## Task 5: Remove Portuguese and runtime language selection

**Files:**
- Delete: `languages/pt-BR.lang`
- Delete: `languages/en-US.lang`
- Modify: `src/Localization.h`
- Modify: `src/main.cpp`
- Modify: `CMakeLists.txt`
- Modify: `build_windows.bat`
- Modify: `.github/workflows/build.yml`
- Modify: `.github/workflows/release.yml`
- Modify: `docs/LOCALIZATION.md`
- Modify: `README.md`
- Modify: `tests/PolicyTests.cpp`

- [ ] Add failing source/package-invariant tests that scan the source tree and assert there is no `pt-BR`, Portuguese alias table, Language menu label, or staged `languages` directory.
- [ ] Confirm RED while the Portuguese file and menu still exist.
- [ ] Reduce `Localizer` to the existing built-in English lookup and remove `AvailableLanguages`, external `.lang` loading, locale persistence, Portuguese aliases, and selection commands.
- [ ] Remove the Language submenu and its command handling from `CreateMenuBar()`/`WndProc`.
- [ ] Remove all language-directory copy steps from CMake, local build scripts, and both workflows.
- [ ] Rewrite `docs/LOCALIZATION.md` as a short statement that this experimental build is English-only; remove obsolete Portuguese instructions from README.
- [ ] Run `rg -n -i "pt-br|portuguese|português|languages\\|language menu" . -g '!downloads/**' -g '!docs/superpowers/**'` and require no obsolete product/build references.
- [ ] Run CTest and a Release application build.
- [ ] Commit:

```text
refactor: make the player English only
```

## Task 6: Runtime-policy integration verification

**Files:**
- Modify if needed: `README.md`
- Modify if needed: `docs/TROUBLESHOOTING.md`

- [ ] Run the complete automated gate:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

- [ ] On the RTX 4080, verify normal launch reports `Neural addon enabled (experimental)` and safe mode reports `DLSS SR safe mode`.
- [ ] With backup copies in a temporary directory, test enabled, disabled, malformed, and read-only `ReShade.ini` states; confirm no unrelated key is lost.
- [ ] Use policy tests—not hardware claims—to verify RTX 50 default enablement and unknown-GPU disablement.
- [ ] Search the produced build tree for Portuguese artifacts and require none.
- [ ] Record the exact test counts and the RTX 50 hardware limitation in the execution handoff.
- [ ] Commit any narrow documentation corrections:

```text
docs: document neural runtime modes
```
