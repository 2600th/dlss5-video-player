# Neural Pre-render Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pre-render every neural frame into a verified persistent cache, then play synchronized original and neural streams with an original-first DLSS comparison toggle.

**Architecture:** Add a small cache/pipeline layer around the current FFmpeg decoder and D3D12 neural renderer. Network media is first materialized locally, every native-resolution frame is evaluated and captured into an encoded cache, and playback consumes paired original/neural decoders under one timeline. Pure selection, cache, progress, and synchronization policies remain independently testable; Win32 orchestration stays in `PlayerApp`.

**Tech Stack:** C++20, Win32/GDI, D3D12, NVIDIA NGX/DLAA, RenoDX/ReShade runtime interception, FFmpeg/FFprobe, yt-dlp, CMake/CTest, Windows BCrypt SHA-256.

**Spec:** `docs/superpowers/specs/2026-09-02-neural-prerender-cache-design.md`

## Global Constraints

- Default neural resolution is exact 1080p; Auto falls back to the highest supported source resolution up to 4K when exact 1080p is absent.
- Manual source choices are only Auto, 1080p, 1440p, and 4K. Remove 480p and 720p commands, enum values, menu items, tests, and documentation.
- Neural input and output dimensions remain equal and `DefaultNeuralCarrierQuality()` remains DLAA; spatial upscaling is off by default.
- Playback starts on Original. The DLSS on/off control selects synchronized Neural rendered/Original frames and does not run live neural evaluation during cached playback.
- Only a complete, hash-valid, decodable cache entry can be reused. Incomplete temporal renders restart from frame zero.
- Public source and release artifacts must exclude leaked, patched, modified, or otherwise unauthorized proprietary runtime files.
- Cache deletion must operate only below a resolved per-user application cache root.
- Toolbar/dialog content insets are at least 10 horizontal DIPs, 6 vertical DIPs, 7 DIPs between icon and label, and 36 DIPs total hit height.
- Do not perform the user-skipped antivirus scan step.
- Do not create the v0.12.0 tag until all automated, package, and Resident Evil acceptance gates pass.

## File map

- `src/RuntimePolicy.h/.cpp`: replace the provisional 720p real-time defaults with 1080p pre-render defaults.
- `src/YouTubeResolver.h/.cpp`: keep only supported qualities, select exact 1080p first, and report available exact heights.
- `src/AppMenu.h/.cpp`: remove 480p/720p commands and add cache clearing.
- `src/NeuralCache.h/.cpp`: SHA-256, cache key, manifest serialization, cache lookup/promotion, size, and safe clearing.
- `src/MediaPipeline.h/.cpp`: owned FFmpeg materialization, raw-frame encoding, probing, cancellation, and encoder fallback classification.
- `src/OfflineNeuralRenderer.h/.cpp`: sequential decode/guide/evaluate/capture/encode job and progress model.
- `src/SynchronizedPlayback.h/.cpp`: paired decoders, shared timestamps, seek transaction, and Original/Neural frame selection.
- `src/D3D12Renderer.h/.cpp`: synchronous neural-output capture to BGRA without applying playback color adjustments.
- `src/UiLayout.h/.cpp` and `src/UiResources.h/.cpp`: pre-render surface layout and DPI-scaled button content geometry.
- `src/main.cpp`: job orchestration, cache hit/miss handling, progress/cancel UI, comparison playback, and cache clear command.
- `tests/NeuralPrerenderTests.cpp`: focused cache, pipeline policy, progress, and synchronization tests.
- `tests/PolicyTests.cpp`: existing menu, resolver, runtime-default, renderer, and UI policy coverage.
- `CMakeLists.txt`: compile new modules, link BCrypt, and register the new test target.
- README, architecture/build/setup/troubleshooting/security/third-party/release notices: document the shipped behavior and measured evidence.

---

### Task 1: Replace provisional low-resolution defaults and remove 480p/720p

**Files:**
- Modify: `src/RuntimePolicy.h`
- Modify: `src/RuntimePolicy.cpp`
- Modify: `src/YouTubeResolver.h`
- Modify: `src/YouTubeResolver.cpp`
- Modify: `src/AppMenu.h`
- Modify: `src/AppMenu.cpp`
- Modify: `src/main.cpp`
- Test: `tests/PolicyTests.cpp`

**Interfaces:**
- Produces: `NeuralRenderDefaults ResolveNeuralRenderDefaults(bool neuralAddonConfigured, bool outputExplicit, uint32_t requestedWidth, uint32_t requestedHeight)`.
- Produces: `YouTubeSourceQuality::{Auto,P2160,P1440,P1080}` only.
- Produces: `std::wstring_view YouTubeFormatSelector(YouTubeSourceQuality)` with exact-height manual selectors and 1080-first Auto fallback.

- [ ] **Step 1: Write failing runtime/menu/selector tests**

Replace the old 720p expectation and six-quality table with:

```cpp
void neural_prerender_defaults_prefer_1080p_and_preserve_explicit_output_test()
{
    CHECK_EQ(NeuralRenderDefaults{1920, 1080},
             ResolveNeuralRenderDefaults(true, false, 3840, 2160));
    CHECK_EQ(NeuralRenderDefaults{2560, 1440},
             ResolveNeuralRenderDefaults(true, true, 2560, 1440));
    CHECK_EQ(NeuralRenderDefaults{3840, 2160},
             ResolveNeuralRenderDefaults(false, false, 3840, 2160));
}

void youtube_quality_surface_has_no_manual_sub_1080p_options_test()
{
    constexpr std::array qualities{
        YouTubeSourceQuality::Auto, YouTubeSourceQuality::P1080,
        YouTubeSourceQuality::P1440, YouTubeSourceQuality::P2160};
    for (const auto quality : qualities) {
        CHECK(app_menu::CommandForYouTubeQuality(quality) != 0);
    }
    CHECK(!app_menu::YouTubeQualityForCommand(414).has_value());
    CHECK(!app_menu::YouTubeQualityForCommand(415).has_value());
}

void youtube_auto_selector_prefers_exact_1080_then_highest_at_or_below_4k_test()
{
    CHECK_EQ(std::wstring_view(
        L"bv[height=1080][ext=mp4]+ba[ext=m4a]/bv[height=1080]+ba/"
        L"b[height=1080]/bv[height<=2160][ext=mp4]+ba[ext=m4a]/"
        L"bv[height<=2160]+ba/b[height<=2160]"),
        YouTubeFormatSelector(YouTubeSourceQuality::Auto));
}
```

Register all three functions in `wmain`.

- [ ] **Step 2: Build PolicyTests and verify RED**

Run:

```powershell
cmake --build build --config Release --target PolicyTests --parallel
```

Expected: compilation fails because `NeuralRenderDefaults` and `ResolveNeuralRenderDefaults` do not exist, and old 480p/720p enum/menu behavior conflicts with the test.

- [ ] **Step 3: Implement the minimal supported-quality policy**

In `RuntimePolicy.h` define:

```cpp
struct NeuralRenderDefaults {
    uint32_t width{};
    uint32_t height{};
    friend bool operator==(const NeuralRenderDefaults&, const NeuralRenderDefaults&) = default;
};
```

Implement:

```cpp
NeuralRenderDefaults ResolveNeuralRenderDefaults(
    bool neuralAddonConfigured, bool outputExplicit,
    uint32_t requestedWidth, uint32_t requestedHeight)
{
    if (neuralAddonConfigured && !outputExplicit) return {1920, 1080};
    return {requestedWidth, requestedHeight};
}
```

Delete `NeuralRealtimeDefaults`, `ResolveNeuralRealtimeDefaults`, and `use720pYouTubeSource`. Remove `P720`, `P480`, `IDM_YOUTUBE_QUALITY_720`, and `IDM_YOUTUBE_QUALITY_480` from headers and switch statements. Make Auto the initial `PlayerApp` source quality and the initial checked menu command. Use exact-height selectors for manual choices and the tested 1080-first selector for Auto.

- [ ] **Step 4: Run the focused tests and inspect the menu**

Run:

```powershell
cmake --build build --config Release --target PolicyTests --parallel
ctest --test-dir build -C Release -R '^PolicyTests$' --output-on-failure
```

Expected: PolicyTests passes, with no source references to `P720`, `P480`, `QUALITY_720`, `QUALITY_480`, `use720pYouTubeSource`, or `ResolveNeuralRealtimeDefaults`.

- [ ] **Step 5: Commit the resolution policy**

```powershell
git add src/RuntimePolicy.h src/RuntimePolicy.cpp src/YouTubeResolver.h src/YouTubeResolver.cpp src/AppMenu.h src/AppMenu.cpp src/main.cpp tests/PolicyTests.cpp
git diff --cached --check
git commit -m "feat: make 1080p the neural render default"
```

---

### Task 2: Add deterministic cache identity, manifests, and safe clearing

**Files:**
- Create: `src/NeuralCache.h`
- Create: `src/NeuralCache.cpp`
- Create: `tests/NeuralPrerenderTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `std::optional<std::string> Sha256File(const std::filesystem::path&, std::stop_token)`.
- Produces: `std::string BuildNeuralCacheKey(const NeuralCacheIdentity&)`.
- Produces: `std::string SerializeNeuralCacheManifest(const NeuralCacheManifest&)` and `std::optional<NeuralCacheManifest> ParseNeuralCacheManifest(std::string_view)`.
- Produces: `NeuralCacheManager::{LookupSource,BeginSourceStaging,PromoteSource,LookupRender,BeginRenderStaging,PromoteRender,MarkInvalid,SizeBytes,Clear}`.
- Produces: `std::optional<std::string> BuildRuntimeDigest(const std::filesystem::path& moduleDirectory, std::span<const std::wstring_view> relativeFiles, std::stop_token)`.

- [ ] **Step 1: Create failing cache tests**

Create a standalone test executable whose `wmain` verifies:

```cpp
void cache_key_changes_for_every_material_input_test()
{
    NeuralCacheIdentity base{
        .sourceDigest="source-a", .width=1920, .height=1080,
        .applicationVersion="0.12.0", .gpuPath="rtx50",
        .runtimeDigest="runtime-a", .quality="DLAA", .upscaling=false};
    const auto key = BuildNeuralCacheKey(base);
    CHECK_EQ(size_t{64}, key.size());
    auto changed = base; changed.sourceDigest = "source-b";
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base; changed.width = 2560;
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base; changed.runtimeDigest = "runtime-b";
    CHECK(key != BuildNeuralCacheKey(changed));
    changed = base; changed.upscaling = true;
    CHECK(key != BuildNeuralCacheKey(changed));
}

void manifest_round_trip_rejects_partial_duplicate_and_unknown_state_test();
void lookup_accepts_only_complete_hash_matching_regular_files_test();
void promotion_is_atomic_and_preserves_an_older_valid_entry_on_failure_test();
void clear_refuses_roots_or_targets_outside_the_resolved_cache_root_test();
void interrupted_staging_is_never_reported_as_resumable_test();
```

Use a per-test directory below `GetTempPathW`, explicit filenames, and scoped cleanup. A complete manifest fixture must contain schema, state, source/render digests, dimensions, frame count, duration, encoder, runtime digest, and `upscaling=false`.

- [ ] **Step 2: Register the target and verify RED**

Add `NeuralPrerenderTests` to CMake with `tests/TestSupport.h`, `src/NeuralCache.cpp`, and `src/NeuralCache.h`; link `bcrypt shell32`. Add `add_test(NAME NeuralPrerenderTests COMMAND NeuralPrerenderTests)`.

Run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DDLSS_SDK=external/DLSS -DFFMPEG_STAGED_DIR=external/ffmpeg/bin
cmake --build build --config Release --target NeuralPrerenderTests --parallel
```

Expected: compilation fails because the cache interfaces do not exist.

- [ ] **Step 3: Implement SHA-256 and strict manifests**

Define the cache types exactly:

```cpp
enum class NeuralCacheState { Staging, Complete, Invalid };

struct NeuralCacheIdentity {
    std::string sourceDigest;
    uint32_t width{}, height{};
    std::string applicationVersion, gpuPath, runtimeDigest, quality;
    bool upscaling{};
};

struct NeuralCacheManifest {
    uint32_t schema{1};
    NeuralCacheState state{NeuralCacheState::Staging};
    std::string sourceDigest, neuralDigest, runtimeDigest, encoder;
    uint32_t width{}, height{};
    uint64_t frameCount{};
    int64_t duration100ns{};
    uint64_t nativeEvaluations{}, observedFeature18Evaluations{};
    bool feature18Created{};
    bool upscaling{};
};
```

Use BCrypt SHA-256 for byte/file hashing. Serialize a deterministic UTF-8 JSON object with one occurrence of each known key. The parser must reject malformed strings, duplicate keys, unknown state strings, missing required keys, dimensions outside 64..7680 by 64..4320, zero frames/duration, `upscaling=true`, or trailing non-whitespace data.

`BuildRuntimeDigest` hashes the actual `nvngx_dlss.dll`, `nvngx_dlssnr.dll`, `dxgi.dll`, `renodx-dlss5.addon64`, and present `sl.*.dll` files in sorted relative-path order. It rejects a missing/non-regular file and hashes the tuple `lowercase relative path + NUL + file SHA-256 + LF`; it never substitutes the expected lock-file hash for bytes actually loaded. Add `DLSS_VIDEO_PLAYER_VERSION="0.12.0"` as a target/test compile definition and include it in every render identity.

- [ ] **Step 4: Implement cache ownership and atomic promotion**

Resolve `%LOCALAPPDATA%\DLSSVideoPlayer\NeuralCache\v1` through `SHGetKnownFolderPath(FOLDERID_LocalAppData)`. Source keys use normalized YouTube video ID plus selected-quality identity; after materialization, the source manifest records and revalidates the local content digest. Render keys use that content digest plus render/runtime settings. `BeginSourceStaging` and `BeginRenderStaging` create `staging\<kind>-<key>-<pid>-<nonce>`. Promotion hashes the payload, writes a complete manifest to staging, verifies it by reopening, and uses a same-volume `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` directory rename only when immutable destination `sources\<key>` or `renders\<key>` does not exist. A valid existing destination wins; an invalid collision is quarantined under the cache root before retry. `Clear` canonicalizes every target with `weakly_canonical`, verifies it is a strict descendant of the cache root, and removes only `sources`, `renders`, and `staging` descendants.

- [ ] **Step 5: Run cache tests GREEN**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
ctest --test-dir build -C Release -R '^NeuralPrerenderTests$' --output-on-failure
```

Expected: all cache identity, manifest, promotion, and safe-clear assertions pass.

- [ ] **Step 6: Commit cache foundations**

```powershell
git add CMakeLists.txt src/NeuralCache.h src/NeuralCache.cpp tests/NeuralPrerenderTests.cpp
git diff --cached --check
git commit -m "feat: add verified neural render cache"
```

---

### Task 3: Materialize network sources and encode cache video with owned FFmpeg jobs

**Files:**
- Create: `src/MediaPipeline.h`
- Create: `src/MediaPipeline.cpp`
- Modify: `tests/NeuralPrerenderTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `MediaMaterializer::Run(const MaterializeRequest&, ProgressCallback, std::stop_token)`.
- Produces: `RawVideoEncoder::{Start,WriteFrame,Finish,Cancel}`.
- Produces: `ProbeResult ProbeMedia(const std::filesystem::path&, std::stop_token)`.
- Produces: `EncoderKind::{HevcNvenc,H264Software}` and `ShouldRetryWithSoftware(EncodeError)`.

- [ ] **Step 1: Add failing process and argument tests**

Add tests using the current test executable copied beside the fixture as fake `ffmpeg.exe`/`ffprobe.exe`:

```cpp
void materializer_arguments_map_video_and_optional_audio_without_a_shell_test();
void materializer_cancel_terminates_only_its_owned_job_tree_test();
void encoder_writes_exact_bgra_frame_bytes_and_closes_stdin_before_wait_test();
void nvenc_initialization_failure_selects_software_h264_but_frame_failure_does_not_publish_test();
void probe_requires_dimensions_frame_count_duration_and_final_frame_decode_test();
void process_errors_and_logs_never_include_ephemeral_input_urls_test();
```

The exact materialization argument vector is:

```text
-hide_banner -nostdin -loglevel error -y -i <video-url> -i <audio-url>
-map 0:v:0 -map 1:a:0? -c copy -f matroska <source.partial.mkv>
```

The exact preferred encoder arguments are:

```text
-hide_banner -nostdin -loglevel error -y -f rawvideo -pix_fmt bgra
-video_size <WxH> -framerate <fps> -i pipe:0 -an -c:v hevc_nvenc
-preset p7 -tune hq -rc vbr -cq 16 -b:v 0 -pix_fmt yuv420p
-f matroska <neural.partial.mkv>
```

The software fallback replaces the codec tail with `-c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p`.

- [ ] **Step 2: Build and verify RED**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
```

Expected: compilation fails on missing `MediaPipeline` types.

- [ ] **Step 3: Implement one owned-process primitive inside MediaPipeline**

Use `CreateProcessW` directly with an explicit argument-vector quoting function, inherited pipe handles restricted through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, a kill-on-close Job Object, bounded wait/poll loops, and `std::stop_token`. Never invoke `cmd.exe`, PowerShell, ShellExecute, or a command shell. Keep URL strings out of error detail and logs; report only phase, Win32 code, and FFmpeg exit code.

- [ ] **Step 4: Implement source materialization, encoder, and probe**

Define:

```cpp
struct MaterializeRequest {
    std::wstring videoUrl, audioUrl;
    std::filesystem::path output;
};
struct EncoderSpec { uint32_t width{}, height{}; double fps{}; EncoderKind kind{}; };
struct ProbeResult {
    bool ok{}; uint32_t width{}, height{}; uint64_t frameCount{};
    int64_t duration100ns{}; bool decodedFinalFrame{}; std::wstring detail;
};
```

Materialization writes only to `.partial.mkv`, verifies the regular file, hashes it, then renames it within staging. Encoder `WriteFrame` rejects any span whose size is not exactly `width*height*4`. `Finish` closes stdin before waiting. `ProbeMedia` uses FFprobe JSON for dimensions/count/duration and a bounded FFmpeg `-v error -sseof -1 -i <file> -frames:v 1 -f null -` final-frame decode.

- [ ] **Step 5: Run the process tests GREEN and leak-check repeated cancellation**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
ctest --test-dir build -C Release -R '^NeuralPrerenderTests$' --output-on-failure
```

Expected: argument vectors match, cancellation returns within the tested bound, and repeated cycles restore process/handle counts.

- [ ] **Step 6: Commit the media pipeline**

```powershell
git add CMakeLists.txt src/MediaPipeline.h src/MediaPipeline.cpp tests/NeuralPrerenderTests.cpp
git diff --cached --check
git commit -m "feat: add owned offline media pipeline"
```

---

### Task 4: Capture evaluated neural output from D3D12

**Files:**
- Modify: `src/D3D12Renderer.h`
- Modify: `src/D3D12Renderer.cpp`
- Modify: `tests/PolicyTests.cpp`

**Interfaces:**
- Produces: `CapturedVideoFrame { std::vector<uint8_t> bgra; uint32_t width; uint32_t height; }`.
- Produces: `bool D3D12Renderer::RenderFrameForCache(..., CapturedVideoFrame&)`.
- Preserves: existing `RenderFrame` and `PresentCurrent` behavior.

- [ ] **Step 1: Add failing renderer capture contract tests**

Extend `D3D12RendererTestAccess` and add tests that assert:

```cpp
void renderer_cache_capture_requires_a_successful_neural_evaluation_test();
void renderer_cache_capture_returns_exact_tight_bgra_geometry_test();
void renderer_cache_capture_wait_failure_never_exposes_partial_bytes_test();
void renderer_cache_capture_does_not_apply_playback_color_adjustments_test();
```

The test access route may inject a capture callback in test builds, but no test-only public API may appear in ReleaseApiCompileTests.

- [ ] **Step 2: Build and verify RED**

```powershell
cmake --build build --config Release --target PolicyTests --parallel
```

Expected: compilation fails because `CapturedVideoFrame` and `RenderFrameForCache` do not exist.

- [ ] **Step 3: Add the GPU conversion/readback resources**

Create one `R8G8B8A8_UNORM` cache render target, its RTV, and one readback buffer with `GetCopyableFootprints`. Increase the RTV heap count accordingly. Reuse the existing linear-to-sRGB presentation shader with identity color constants so cache output contains neural pixels only; runtime color controls remain non-destructive playback adjustments.

- [ ] **Step 4: Refactor frame submission and implement synchronous capture**

Factor the shared implementation into:

```cpp
bool RenderFrameInternal(const uint8_t* bgra, size_t bytes,
                         const float* guides, size_t guideBytes,
                         uint32_t gridW, uint32_t gridH,
                         bool temporalReset, float frameTimeMs,
                         CapturedVideoFrame* capture);
```

When `capture` is non-null, require `m_lastDLSSUsed`, render `m_dlssOutput` into the cache RT, transition it to `COPY_SOURCE`, copy it to the readback buffer, submit, signal, wait for that exact fence, map the padded rows, and copy tightly into `width*height*4` BGRA. Clear `capture->bgra` before every attempt and on every failure.

- [ ] **Step 5: Run renderer and full policy tests GREEN**

```powershell
cmake --build build --config Release --target PolicyTests --parallel
ctest --test-dir build -C Release -R '^PolicyTests$' --output-on-failure
```

Expected: existing presentation/fence tests and new capture-contract tests all pass.

- [ ] **Step 6: Commit GPU capture**

```powershell
git add src/D3D12Renderer.h src/D3D12Renderer.cpp tests/PolicyTests.cpp
git diff --cached --check
git commit -m "feat: capture evaluated neural frames"
```

---

### Task 5: Implement the offline neural render job and progress model

**Files:**
- Create: `src/OfflineNeuralRenderer.h`
- Create: `src/OfflineNeuralRenderer.cpp`
- Modify: `tests/NeuralPrerenderTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `VideoDecoder`, `TemporalGuideGenerator`, `D3D12Renderer::RenderFrameForCache`, and `RawVideoEncoder`.
- Produces: `NeuralRenderRequest`, `NeuralRenderProgress`, `NeuralRenderResult`, `NeuralRenderPhase`, and `OfflineNeuralRenderer::Run`.
- Produces: `NeuralRuntimeEvidence ParseNeuralRuntimeEvidence(std::string_view reshadeLogSegment)`.

- [ ] **Step 1: Write failing job-state/progress tests with fakes**

Add injectable narrow interfaces `IFrameSource`, `INeuralFrameEvaluator`, and `IFrameEncoder` under `OFFLINE_NEURAL_RENDERER_TESTING`. Test:

```cpp
void offline_job_primes_feature_then_restarts_source_and_captures_every_frame_test();
void offline_job_rejects_any_frame_without_a_neural_evaluation_test();
void offline_job_reports_monotonic_progress_and_smoothed_eta_test();
void offline_job_cancel_stops_before_promotion_and_marks_result_cancelled_test();
void offline_job_nvenc_start_failure_restarts_from_frame_zero_with_h264_test();
void offline_job_does_not_retry_a_temporal_render_from_an_arbitrary_frame_test();
void reshade_evidence_requires_upscaling_off_feature18_create_and_evaluate_test();
void reshade_evidence_rejects_a_later_feature18_failure_in_the_same_job_segment_test();
```

Use five deterministic frames with timestamps `0, 333333, 666666, 999999, 1333332` and assert exactly five encoded frames after priming.

- [ ] **Step 2: Build and verify RED**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
```

Expected: compilation fails on the missing offline-render interfaces.

- [ ] **Step 3: Implement the public job model**

Define:

```cpp
enum class NeuralRenderPhase { Acquiring, Decoding, NeuralRendering, Encoding, Validating, Ready };
struct NeuralRenderRequest {
    HWND renderWindow{};
    std::filesystem::path sourcePath, stagingVideoPath;
    uint32_t width{}, height{};
    double fps{}, durationSeconds{};
};
struct NeuralRenderProgress {
    NeuralRenderPhase phase{}; uint64_t completedFrames{}, totalFrames{}, bytes{};
    std::chrono::milliseconds elapsed{}, estimatedRemaining{};
};
struct NeuralRenderResult {
    bool ok{}, cancelled{}; EncoderKind encoder{}; uint64_t frameCount{};
    int64_t duration100ns{}; uint64_t nativeEvaluations{};
    NeuralRuntimeEvidence evidence{}; std::wstring detail;
};

struct NeuralRuntimeEvidence {
    bool upscalingOff{}, feature18Created{}, feature18Evaluated{}, laterFailure{};
    uint64_t highestObservedEvaluation{};
};
```

Progress callbacks are value snapshots and never borrow frame buffers or Win32 handles.

- [ ] **Step 4: Implement prime, full replay, fallback, and validation handoff**

Record the byte length of module-local `ReShade.log` immediately before renderer initialization. Open the source at native dimensions, initialize a DLAA renderer with equal input/output dimensions, and feed the first decoded frame until the feature is created without encoding those warm-up submissions. Seek/reopen to zero, reset temporal guides, and capture every source frame exactly once. Require the native evaluation counter to advance for each captured frame.

On completion, boundedly wait for ReShade to flush and parse only bytes appended after the recorded offset. Accept evidence only when the segment contains `active settings: upscaling=OFF`, a successful `feature 18 created` line, and an `inline feature 18 evaluation succeeded` line, with no later `feature 18 create failed` or evaluation-failure line. Store the highest logged evaluation count as supporting evidence; exact every-frame accounting comes from native submitted/captured frame counts because the add-on logs periodically.

If any NVENC start, write, or finish operation fails, delete only that staging encode, reopen the decoder, reset temporal state, and rerun the entire frame sequence with software H.264. A software-encoder failure or missing/failed feature-18 evidence returns failure and leaves staging unpromoted.

- [ ] **Step 5: Run offline job tests GREEN**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
ctest --test-dir build -C Release -R '^NeuralPrerenderTests$' --output-on-failure
```

Expected: exact frame coverage, monotonic progress, cancellation, and full-restart fallback pass.

- [ ] **Step 6: Commit the offline renderer**

```powershell
git add CMakeLists.txt src/OfflineNeuralRenderer.h src/OfflineNeuralRenderer.cpp tests/NeuralPrerenderTests.cpp
git diff --cached --check
git commit -m "feat: render neural video before playback"
```

---

### Task 6: Add synchronized original/neural playback

**Files:**
- Create: `src/SynchronizedPlayback.h`
- Create: `src/SynchronizedPlayback.cpp`
- Modify: `tests/NeuralPrerenderTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `ComparisonView::{Original,Neural}`.
- Produces: `SynchronizedFramePair`, `SynchronizedReadResult`, and `SynchronizedPlayback::{Open,ReadNextAvailable,SeekSeconds,SetView,VisibleFrame,Close}`.
- Guarantees: visible original and neural PTS differ by no more than one source-frame duration.

- [ ] **Step 1: Write failing paired-timeline tests**

Add fake decoder tests:

```cpp
void synchronized_playback_starts_original_and_switches_same_timestamp_test();
void synchronized_playback_advances_both_streams_under_one_clock_test();
void synchronized_playback_seek_commits_only_after_both_streams_reach_target_test();
void synchronized_playback_refuses_a_mismatched_neural_frame_beyond_one_frame_test();
void synchronized_playback_pause_step_and_eos_apply_to_both_streams_test();
void synchronized_playback_original_only_mode_remains_available_after_cancel_test();
```

- [ ] **Step 2: Build and verify RED**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
```

Expected: compilation fails on missing synchronized playback types.

- [ ] **Step 3: Implement paired decoder ownership**

Define:

```cpp
enum class ComparisonView { Original, Neural };
struct SynchronizedFramePair { VideoFrame original, neural; int64_t timestamp100ns{}; };
enum class SynchronizedReadResult { PairReady, NotReady, EndOfStream, Error, Cancelled, OutOfSync };
```

`Open(originalPath, neuralPath)` opens two local `VideoDecoder`s and verifies equal dimensions, frame rates within 0.01 fps, and durations within one frame. `ReadNextAvailable` holds the earlier frame and advances only that decoder until timestamps match within `ceil(10'000'000/fps)`. `SetView(Neural)` succeeds only when a neural decoder and a synchronized pair exist. `SeekSeconds` prepares both decoders and publishes the new position only when both seek calls and first post-seek frames succeed.

- [ ] **Step 4: Run synchronization tests GREEN**

```powershell
cmake --build build --config Release --target NeuralPrerenderTests --parallel
ctest --test-dir build -C Release -R '^NeuralPrerenderTests$' --output-on-failure
```

Expected: same-timestamp switching and transactional seek tests pass.

- [ ] **Step 5: Commit synchronized playback**

```powershell
git add CMakeLists.txt src/SynchronizedPlayback.h src/SynchronizedPlayback.cpp tests/NeuralPrerenderTests.cpp
git diff --cached --check
git commit -m "feat: synchronize original and neural playback"
```

---

### Task 7: Add pre-render UI, cache clearing, and correct button padding

**Files:**
- Modify: `src/UiLayout.h`
- Modify: `src/UiLayout.cpp`
- Modify: `src/UiResources.h`
- Modify: `src/UiResources.cpp`
- Modify: `src/AppMenu.h`
- Modify: `src/AppMenu.cpp`
- Modify: `src/Localization.h`
- Modify: `tests/PolicyTests.cpp`

**Interfaces:**
- Produces: `ButtonContentLayout LayoutButtonContent(RECT, SIZE icon, SIZE text, bool stacked, UINT dpi)`.
- Produces: `PreRenderSurfaceLayout LayoutPreRenderSurface(int width, int height, UINT dpi)`.
- Produces: `IDM_CLEAR_NEURAL_CACHE`.

- [ ] **Step 1: Write failing DPI/button/progress/menu tests**

Add table-driven tests at 96, 120, 144, and 192 DPI:

```cpp
void button_content_layout_preserves_required_insets_and_icon_gap_at_every_dpi_test();
void button_content_layout_centers_combined_icon_and_label_without_outline_contact_test();
void prerender_surface_layout_keeps_progress_cancel_and_text_inside_client_bounds_test();
void advanced_menu_contains_clear_neural_cache_and_no_removed_quality_commands_test();
```

For non-stacked icon+label layout assert left/right content distance is at least `MulDiv(10,dpi,96)`, top/bottom distance at least `MulDiv(6,dpi,96)`, icon/text gap at least `MulDiv(7,dpi,96)`, and bounds height at least `MulDiv(36,dpi,96)`.

- [ ] **Step 2: Build and verify RED**

```powershell
cmake --build build --config Release --target PolicyTests --parallel
```

Expected: compilation fails on the missing layout types/functions and cache command.

- [ ] **Step 3: Implement measured button content geometry**

Add constants `kButtonHorizontalInsetDip=10`, `kButtonVerticalInsetDip=6`, and `kButtonIconLabelGapDip=7`. `LayoutButtonContent` returns separate icon/text rectangles after shrinking the outer rectangle by the scaled insets; combined content is centered, and text is ellipsized only inside its content rectangle. Update `PlayerApp::DrawButton` to call this function instead of the current `Dip(1)`, `Dip(2)`, and `Dip(3)` offsets. Preserve visual/focus state drawing.

- [ ] **Step 4: Implement the pre-render surface and menu command**

`PreRenderSurfaceLayout` supplies title, phase, resolution, frame count, elapsed/ETA, size, progress track/fill, and a 120x40 DIP Cancel button. Add localized English strings for Acquiring, Neural rendering, Encoding, Validating, Ready, Cancel, Clear Neural Cache, Original, Neural rendered, and synchronization warning. Add `IDM_CLEAR_NEURAL_CACHE` under Advanced.

- [ ] **Step 5: Run UI policy tests GREEN**

```powershell
cmake --build build --config Release --target PolicyTests --parallel
ctest --test-dir build -C Release -R '^PolicyTests$' --output-on-failure
```

Expected: layout/menu tests and all existing UI tests pass.

- [ ] **Step 6: Commit UI work**

```powershell
git add src/UiLayout.h src/UiLayout.cpp src/UiResources.h src/UiResources.cpp src/AppMenu.h src/AppMenu.cpp src/Localization.h tests/PolicyTests.cpp
git diff --cached --check
git commit -m "fix: pad controls and add neural render progress"
```

---

### Task 8: Integrate automatic cache rendering and comparison into PlayerApp

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/UiLayout.h`
- Modify: `src/UiLayout.cpp`
- Modify: `src/RuntimePolicy.h`
- Modify: `src/RuntimePolicy.cpp`
- Modify: `src/YouTubeResolver.h`
- Modify: `src/YouTubeResolver.cpp`
- Modify: `tests/PolicyTests.cpp`
- Modify: `tests/NeuralPrerenderTests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: every interface produced by Tasks 1-7.
- Produces: `NeuralPlaybackLifecycle` pure state transitions for acquiring, rendering, validating, ready, original-only, cancelling, and failed states.
- Produces: PlayerApp messages `WM_NEURAL_PROGRESS` and `WM_NEURAL_COMPLETE` backed by `CompletionRegistry` ownership.
- Produces: `YouTubeFormatAvailability ParseYouTubeFormatMetadata(std::string_view json)` and `YouTubeResolver::InspectFormats(std::wstring_view, std::stop_token)`.

- [ ] **Step 1: Write failing lifecycle and orchestration policy tests**

Add pure tests:

```cpp
void neural_open_uses_valid_cache_without_starting_a_job_test();
void neural_open_starts_materialize_then_render_on_cache_miss_test();
void neural_open_bypasses_prerender_when_runtime_is_absent_or_safe_mode_test();
void neural_completion_publishes_only_after_probe_and_manifest_validation_test();
void neural_cancel_and_failure_offer_original_only_without_partial_cache_test();
void dlss_toggle_in_cached_playback_changes_comparison_view_not_renderer_feature_test();
void source_change_cancels_and_joins_the_owned_job_before_replacement_test();
void cache_clear_reports_size_and_targets_only_the_manager_root_test();
```

- [ ] **Step 2: Build and verify RED**

```powershell
cmake --build build --config Release --target PolicyTests NeuralPrerenderTests --parallel
```

Expected: compilation fails on missing lifecycle policy.

- [ ] **Step 3: Implement lifecycle policy and worker ownership**

Define:

```cpp
enum class NeuralPlaybackState {
    Idle, Acquiring, Rendering, Validating, Ready, OriginalOnly, Cancelling, Failed
};
struct NeuralPlaybackLifecycle {
    NeuralPlaybackState state{NeuralPlaybackState::Idle};
    uint64_t generation{};
    uint64_t Begin();
    bool Accept(uint64_t candidateGeneration) const;
    bool Transition(NeuralPlaybackState next);
    void Invalidate();
};
```

Permit only the state edges described by the spec. `PlayerApp` owns one `std::jthread` render worker, one completion registry, and immutable progress snapshots posted to the UI thread. Opening another source, closing, or Cancel requests stop, cancels owned helpers, joins, invalidates the generation, and then replaces state.

- [ ] **Step 4: Route local and YouTube opens through cache preparation**

For a complete external neural runtime, local open computes the source digest and render identity; YouTube open resolves Auto/manual format, materializes `source.mkv` under a source-cache staging entry, hashes it, and derives the render key. A valid hit opens synchronized playback immediately. A miss creates a render staging entry and invokes `OfflineNeuralRenderer`. Runtime absent/safe mode retains the existing original-only open path.

Add a bounded metadata inspection call using the same verified yt-dlp/Deno process policy and these arguments:

```text
--no-config --no-cache-dir --no-plugin-dirs --no-playlist --no-warnings
--js-runtimes deno:<package-local-deno> --skip-download --print %(formats)j <youtube-url>
```

Cap captured metadata at 4 MiB. `ParseYouTubeFormatMetadata` is a strict JSON scanner that extracts positive integer `height` values from format objects while correctly skipping escaped string contents; it sets `p1080`, `p1440`, and `p2160` only for exact heights and rejects malformed/trailing data. Enable exact menu entries only for true flags. Auto remains enabled and follows the 1080-first selector. Add tests for escaped strings, duplicate heights, null heights, malformed JSON, output overflow, and a catalog containing only 720p (Auto enabled, all manual entries disabled).

- [ ] **Step 5: Install cached playback and redefine the DLSS toggle**

After validation/promotion, create a normal presentation renderer with equal source/output dimensions and `SetDLSS(false)`. Open `SynchronizedPlayback(original, neural)`, set `ComparisonView::Original`, and start audio from the original source. In cached mode, `ToggleDLSS` calls `SetView(Neural/Original)` and never calls `D3D12Renderer::SetDLSS(true)`. `ButtonContent` reports `DLSS off · Original` or `DLSS on · Neural rendered`. Seek/pause/step/stop act on the synchronized playback and original audio clock.

- [ ] **Step 6: Render progress/cancel and implement safe cache clear**

While lifecycle state is Acquiring/Rendering/Validating, hide playback controls, paint `PreRenderSurfaceLayout`, update it from progress snapshots, and route Escape/Cancel only to the owned job. `IDM_CLEAR_NEURAL_CACHE` calls `SizeBytes`, displays the formatted recoverable size, and calls `Clear` only after user confirmation. Refresh the UI after completion or failure.

- [ ] **Step 7: Run all automated tests GREEN**

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Expected: PolicyTests, NeuralPrerenderTests, and ReleaseApiCompileTests all pass.

- [ ] **Step 8: Commit PlayerApp integration**

```powershell
git add CMakeLists.txt src/main.cpp src/UiLayout.h src/UiLayout.cpp src/RuntimePolicy.h src/RuntimePolicy.cpp src/YouTubeResolver.h src/YouTubeResolver.cpp tests/PolicyTests.cpp tests/NeuralPrerenderTests.cpp
git diff --cached --check
git commit -m "feat: play verified neural caches with comparison"
```

---

### Task 9: Update package policy and public documentation

**Files:**
- Modify: `README.md`
- Modify: `SECURITY.md`
- Modify: `THIRD_PARTY.md`
- Modify: `docs/ARCHITECTURE.md`
- Modify: `docs/BUILDING.md`
- Modify: `docs/DLSS5_SETUP.md`
- Modify: `docs/RELATED_PROJECTS.md`
- Modify: `docs/TROUBLESHOOTING.md`
- Modify: `packaging/PUBLIC_RELEASE_NOTICE.txt`
- Modify: `packaging/EXPERIMENTAL_RUNTIME_NOTICE.txt`
- Modify: `src/ReleasePackagePolicy.h`
- Modify: `tools/package_release.ps1`
- Modify: `tools/verify_package.ps1`
- Modify: `.github/workflows/release.yml`
- Test: `tests/PolicyTests.cpp`

**Interfaces:**
- Preserves: public-core allowlist excludes FFmpeg/YouTube helpers and every private experimental neural runtime file.
- Produces: documentation matching actual Auto, cache, comparison, fallback, and verification behavior.

- [ ] **Step 1: Add/adjust failing release policy assertions**

Assert the public allowlist contains only the approved core files, excludes `nvngx_dlssnr.dll`, `renodx-dlss5.addon64`, `dxgi.dll`, all `sl.*.dll`, cache/test media/logs, and does not acquire new runtime binaries. Assert package documentation includes `PUBLIC_RELEASE_NOTICE.txt` and the cache behavior docs.

- [ ] **Step 2: Run PolicyTests and verify RED where documentation/package expectations changed**

```powershell
cmake --build build --config Release --target PolicyTests --parallel
ctest --test-dir build -C Release -R '^PolicyTests$' --output-on-failure
```

Expected: the new content/policy assertions fail until notices and allowlists agree.

- [ ] **Step 3: Update release policy and workflow**

Keep the public artifact name `DLSSVideoPlayer-v0.12.0-core-win64.zip`. Do not add private runtime files, FFmpeg, yt-dlp, Deno, cached videos, or application logs. Keep the private experimental assembler hash-locked and explicitly non-publishable without redistribution rights. The workflow builds/tests the new modules but publishes only the verified public-core ZIP.

- [ ] **Step 4: Rewrite behavior and limitations documentation**

Document: Auto prefers exact 1080p then highest supported up to 4K; 480p/720p manual options are gone; native-resolution DLAA keeps upscaling off; a full verified disk cache is required before neural playback; YouTube source material is private cache data; DLSS off/on means Original/Neural rendered in cached mode; Clear Neural Cache behavior; software encoder fallback; no arbitrary temporal resume; current runtime provenance boundary; RTX 50 local test scope and unverified RTX 40 hardware scope.

- [ ] **Step 5: Run tests and package validation GREEN**

```powershell
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
.\package_public_release.bat
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_package.ps1 -StageDir .\dist\DLSSVideoPlayer-v0.12.0-core-win64 -PublicCore
```

Expected: all tests pass, package verifier reports exact allowlist success, and no private experimental runtime file appears in stage or ZIP.

- [ ] **Step 6: Commit documentation and packaging**

```powershell
git add README.md SECURITY.md THIRD_PARTY.md docs packaging src/ReleasePackagePolicy.h tools .github/workflows/release.yml tests/PolicyTests.cpp
git diff --cached --check
git commit -m "docs: publish neural prerender release guidance"
```

---

### Task 10: Run the Resident Evil acceptance gate, review, and tag

**Files:**
- Verify: `build/Release/DLSSVideoPlayer.exe`
- Verify: `build/Release/DLSSVideoPlayer.log`
- Verify: per-user neural cache manifest/artifacts
- Verify: `dist/DLSSVideoPlayer-v0.12.0-core-win64.zip`
- Modify only if measured evidence differs: README and relevant docs

**Interfaces:**
- Consumes: completed implementation and package.
- Produces: measured 1080p evidence, review result, final commit, and annotated local tag `v0.12.0` only after every gate succeeds.

- [ ] **Step 1: Perform a clean build and full automated verification**

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON -DDLSS_SDK=external/DLSS -DFFMPEG_STAGED_DIR=external/ffmpeg/bin
cmake --build build --config Release --clean-first --parallel
ctest --test-dir build -C Release --output-on-failure
git diff --check
```

Expected: clean compile and every CTest target passes.

- [ ] **Step 2: Run full Resident Evil 1080p pre-render**

Open `https://www.youtube.com/watch?v=9lrThxCoznw` through the built player with the complete locally supplied experimental runtime. Confirm Auto selects 1920x1080, the pre-render view remains responsive, progress reaches validation, and the completed manifest records `upscaling=false`, feature 18 evidence, exact dimensions, encoder, frame count, runtime digest, and complete state.

- [ ] **Step 3: Validate the cached artifact independently**

Run FFprobe/FFmpeg against the manifest-selected neural file:

```powershell
$renderRoot = Join-Path $env:LOCALAPPDATA 'DLSSVideoPlayer\NeuralCache\v1\renders'
$manifestFile = Get-ChildItem -LiteralPath $renderRoot -Filter manifest.json -File -Recurse |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $manifestFile) { throw 'No completed neural cache manifest was found.' }
$manifestData = Get-Content -LiteralPath $manifestFile.FullName -Raw | ConvertFrom-Json
if ($manifestData.state -ne 'complete') { throw 'Newest neural cache entry is not complete.' }
$neuralFile = Join-Path $manifestFile.DirectoryName 'neural.mkv'
if (-not (Test-Path -LiteralPath $neuralFile -PathType Leaf)) { throw 'Manifest neural video is missing.' }
.\build\Release\ffprobe.exe -v error -count_frames -select_streams v:0 -show_entries stream=width,height,avg_frame_rate,nb_read_frames:format=duration -of json $neuralFile
.\build\Release\ffmpeg.exe -v error -i $neuralFile -map 0:v:0 -f null NUL
```

Expected: 1920x1080, expected full frame count, duration within one source frame, and no decode error. Inspect the application log for successful feature-18 creation and evaluations through the final frame plus `upscaling=OFF`.

- [ ] **Step 4: Exercise playback and comparison**

Play start-to-finish, pause, seek to multiple positions, frame-step, and toggle DLSS off/on repeatedly. Confirm playback starts Original, every switch shows the same timestamp, audio remains synchronized, and no synchronization warning appears under normal operation. Record total/warm-up-excluded dropped frames; require no sustained drops and no more than 0.5% after the first two seconds.

- [ ] **Step 5: Prove cache reuse**

Close and reopen the same URL/settings/runtime. Confirm playback reaches Ready through a cache hit without running neural evaluation or rewriting the completed artifact.

- [ ] **Step 6: Inspect UI at required DPI scales**

At 100%, 125%, 150%, and 200% Windows scaling, inspect idle, pre-render, playback, hover, pressed, focused, and disabled controls. Confirm icon/label insets and gap remain clear and no content touches button outlines.

- [ ] **Step 7: Request code review and resolve findings**

Use `superpowers:requesting-code-review` against the full branch diff. Address every correctness, safety, synchronization, cache-boundary, and release-policy finding, then rerun Steps 1-6 for affected areas.

- [ ] **Step 8: Rebuild and verify the final public package**

```powershell
.\package_public_release.bat
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_package.ps1 -StageDir .\dist\DLSSVideoPlayer-v0.12.0-core-win64 -PublicCore
Get-FileHash -Algorithm SHA256 .\dist\DLSSVideoPlayer-v0.12.0-core-win64.zip
```

Expected: exact public-core allowlist passes and a final SHA-256 is recorded. The antivirus scan remains skipped as requested.

- [ ] **Step 9: Create the final evidence commit and annotated local tag**

First verify no unexpected files are staged and all desired implementation/docs changes are committed:

```powershell
git status --short
git log -1 --oneline
git tag --list v0.12.0
```

If the tag list is empty and every preceding gate passed:

```powershell
git tag -a v0.12.0 -m "DLSS Video Player v0.12.0"
git show --no-patch --decorate v0.12.0
```

Do not push the commit or tag unless the user separately requests a push.
