# YouTube Streaming and Curated Examples Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users paste or choose a public YouTube video and stream a single combined audio/video format through the existing FFmpeg paths without downloading the video or freezing the UI.

**Architecture:** Validate YouTube input with pure functions, resolve it in a cancellable worker using a directly spawned pinned `yt-dlp.exe` plus pinned Deno, then pass the ephemeral HTTPS media URL to the existing decoder/audio APIs. Compile six stable official examples into the app; do not scrape or update them at runtime.

**Tech Stack:** C++20, Win32, WinHTTP URL parsing, `CreateProcessW`, job objects, FFmpeg, yt-dlp 2026.08.19, Deno 2.9.5, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-08-31-release-ui-youtube-design.md`

## Global Constraints

- Execute after the runtime and UI plans in the same isolated worktree.
- Use `superpowers:test-driven-development` and apply `superpowers:systematic-debugging` to resolver/process failures.
- Public, non-DRM, on-demand content only; no cookies, login extraction, browser control, DRM bypass, or permanent cache.
- Never use `cmd.exe`, PowerShell, `ShellExecute`, or any command shell to resolve a user URL.
- Never write or log the ephemeral direct media URL; it may contain temporary tokens.
- Bound input, helper output, helper lifetime, and cancellation.
- Availability of example videos is external and may change; failure must not affect local playback.

---

## Task 1: Pin the YouTube helper binaries reproducibly

**Files:**
- Create: `tools/fetch_youtube_helpers.ps1`
- Create: `external/youtube/README.md`
- Create on fetch: `external/youtube/yt-dlp.exe`
- Create on fetch: `external/youtube/deno.exe`
- Modify: `.gitignore`
- Modify: `THIRD_PARTY.md`

- [ ] Add a fetch script with these immutable inputs:

```text
yt-dlp.exe
URL: https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp.exe
SHA-256: 66674953FE251B89F4D08C5F0E35E0728679BD67AB3D7D05C0562AF101DD3E7A

Deno Windows x86_64 zip
URL: https://github.com/denoland/deno/releases/download/v2.9.5/deno-x86_64-pc-windows-msvc.zip
SHA-256: 171EFAB55AC6B9881FD53EE4C20F8BF3BB1340FFC618483746909014DB12216A
```

- [ ] Download to a newly created temporary directory, verify SHA-256 before extraction/copy, and fail closed on mismatch.
- [ ] Keep fetched executables ignored in source control; commit only the script and provenance README.
- [ ] Document yt-dlp's Unlicense and Deno's MIT license in `THIRD_PARTY.md`; note that the official yt-dlp executable embeds its EJS support while Deno supplies the JavaScript runtime.
- [ ] Run the script and verify:

```powershell
external\youtube\yt-dlp.exe --version
external\youtube\deno.exe --version
```

- [ ] Require exact versions `2026.08.19` and `deno 2.9.5`.
- [ ] Commit:

```text
build: pin YouTube resolver helpers
```

## Task 2: Add strict input and output validation

**Files:**
- Create: `src/YouTubeResolver.h`
- Create: `src/YouTubeResolver.cpp`
- Modify: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Write failing table tests for accepted URLs: `https://youtube.com/watch?v=...`, `www`, `m`, `music`, `youtu.be/<id>`, `shorts/<id>`, and watch URLs that also contain a playlist parameter.
- [ ] Write failing rejection tests for HTTP, user-info, lookalike suffixes, arbitrary domains, file/UNC paths, controls, quotes, whitespace-only input, over-2048 characters, playlist-only URLs, and missing video IDs.
- [ ] Write failing output tests for empty/multiple/oversize output, CRLF trimming, error text, non-HTTPS URLs, and non-`googlevideo.com` result hosts.
- [ ] Define:

```cpp
enum class ResolveError {
    None, InvalidUrl, HelperMissing, StartFailed, TimedOut,
    Cancelled, ExtractionFailed, InvalidOutput
};

struct ResolveResult {
    bool ok{false};
    std::wstring mediaUrl;
    ResolveError error{ResolveError::None};
    std::wstring detail;
};

bool IsSupportedYouTubeUrl(std::wstring_view value);
ResolveResult ParseResolverOutput(std::string_view stdoutBytes, DWORD exitCode);
```

- [ ] Confirm RED before production implementation.
- [ ] Use `WinHttpCrackUrl` for normalized scheme/host/path parsing; accept host exactly `youtu.be`, exactly `youtube.com`, or a dot-bound subdomain of `youtube.com`.
- [ ] Require HTTPS, a selected video ID, no control/quote characters, and length 1–2048.
- [ ] For resolver output, accept one nonempty line up to 16 KiB whose parsed HTTPS host is exactly `googlevideo.com` or a dot-bound subdomain; retain at most 4 KiB of sanitized non-URL diagnostic detail.
- [ ] Add `winhttp` to app/test links, run CTest, and commit:

```text
feat: validate YouTube resolver inputs
```

## Task 3: Implement bounded, cancellable resolution

**Files:**
- Modify: `src/YouTubeResolver.h`
- Modify: `src/YouTubeResolver.cpp`
- Modify: `tests/PolicyTests.cpp`

- [ ] Add failing tests for Windows argument quoting and the exact immutable argument vector:

```text
--no-config
--no-playlist
--no-warnings
--js-runtimes deno:<absolute-exe-directory>\deno.exe
-f b[ext=mp4]/b
--get-url
<validated-user-url>
```

- [ ] Confirm RED, then expose a single resolver class:

```cpp
class YouTubeResolver {
public:
    ResolveResult Resolve(std::wstring_view youtubeUrl, std::stop_token stop);
    void Cancel();
};
```

- [ ] Discover `yt-dlp.exe` and `deno.exe` only beside the app; do not search PATH in a release run.
- [ ] Construct a mutable command line with a tested Windows quoting helper and call `CreateProcessW` with `lpApplicationName` set to the absolute yt-dlp path, `CREATE_NO_WINDOW`, inherited stdout/stderr pipe only, and no shell.
- [ ] Put the child in a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so only this app's helper tree is terminated.
- [ ] Drain output without blocking the UI, cap captured bytes at 64 KiB, enforce a 45-second deadline, and poll cancellation at no more than 50 ms intervals.
- [ ] On cancel/timeout/overflow, terminate the job, wait a bounded 2 seconds, close all handles, and return the precise error.
- [ ] Do not include the input URL, resolved URL, or raw helper command line in logs.
- [ ] Add a test-only fake child process mode or injectable process runner to prove timeout/cancel/overflow cleanup without internet.
- [ ] Run CTest repeatedly and inspect Task Manager/handle counts for leaked helpers.
- [ ] Commit:

```text
feat: resolve YouTube streams safely
```

## Task 4: Add the modal flow and responsive worker integration

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/Localization.h`

- [ ] Add `File > Open YouTube URL…` and connect the idle-screen action from the UI plan.
- [ ] Implement a small owned modal dialog using native controls: URL edit, Paste, Play, Cancel, and `Public, non-DRM videos only.` Keep focus in the edit and make Enter/Escape work.
- [ ] Validate before dismissing the dialog; show inline/actionable validation rather than spawning a helper for invalid input.
- [ ] Store resolver work in a `std::jthread`; immediately set the status to `Resolving YouTube…`, disable conflicting open/example actions, and keep paint/menu/message processing responsive.
- [ ] Post a private `WM_APP + n` completion message containing an owned result object; verify the window generation/token before applying it so stale results cannot load after a new source.
- [ ] Cancel and join resolver work when opening another source, stopping a resolution, destroying the window, or exiting. Never block the UI thread beyond the resolver's bounded cleanup.
- [ ] On success, call a refactored `Load(source, displayTitle, sourceKind)` so the same direct URL feeds both `VideoDecoder` and `AudioPlayer` while the UI displays the YouTube title/example label rather than the tokenized URL.
- [ ] Force the URL path through FFmpeg; do not fall back to Media Foundation for an ephemeral network URL.
- [ ] On failure, distinguish invalid input, helper missing, startup failure, extraction error, timeout, cancellation, and FFmpeg open failure; keep local-file actions usable.
- [ ] Review all log sites and prove no direct media URL is emitted.
- [ ] Commit:

```text
feat: open YouTube videos in the player
```

## Task 5: Compile six official examples into the app

**Files:**
- Create: `src/ExampleVideos.h`
- Modify: `src/main.cpp`
- Modify: `tests/PolicyTests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `README.md`

- [ ] Add a constexpr table with exactly these entries and no runtime trending lookup:

```text
Games
GTA VI Trailer 2 | Rockstar Games | https://www.youtube.com/watch?v=VQRLujxTm3c
Resident Evil Requiem - Launch Trailer | Resident Evil | https://www.youtube.com/watch?v=9lrThxCoznw
Battlefield 6 Season 3 Official Gameplay Trailer | Battlefield | https://www.youtube.com/watch?v=XCMr55EjFew

Anime
2026 Summer Anime Season Trailer | Crunchyroll | https://www.youtube.com/watch?v=DWM2IfkzLHo
Spring 2026 Season Official Trailer | Crunchyroll | https://www.youtube.com/watch?v=7Wc6ugY3meg
My Hero Academia FINAL SEASON "More" Official Trailer | Crunchyroll | https://www.youtube.com/watch?v=pxbEWUjh6E4
```

- [ ] Add tests for exactly three Games and three Anime entries, unique URLs, supported URL validation, nonempty titles/channels, and HTTPS watch URLs.
- [ ] Add `Examples > Games` and `Examples > Anime` submenus; choosing one enters the same resolver flow as pasted input.
- [ ] Document the fixed list in README and state that availability/region access can change.
- [ ] Run a clean resolver smoke test for all six at implementation time; if an entry has become unavailable, replace it only with another verified official public video and update test/docs/table together.
- [ ] Commit:

```text
feat: add official video examples
```

## Task 6: YouTube behavior verification

**Files:**
- Modify if needed: `docs/TROUBLESHOOTING.md`

- [ ] Run the full CTest and Release build gate.
- [ ] Resolve and play at least one game and one anime example for two minutes each; verify video, synchronized audio, play/pause, seek, mute, stop, aspect, DLSS toggle, and fullscreen.
- [ ] Verify no media/cache file is created in the app directory, user Downloads, or `%TEMP%` by the player; helper runtime cache must be prevented or confined to a package-local disposable helper cache excluded from the ZIP.
- [ ] Try invalid domain, playlist-only, unavailable/private, and deliberately cancelled inputs; confirm responsive errors and no lingering `yt-dlp.exe`, `deno.exe`, `ffmpeg.exe`, or `ffprobe.exe` children.
- [ ] Inspect `DLSSVideoPlayer.log` and require that neither `googlevideo.com` nor URL query tokens appear.
- [ ] Disconnect networking and verify local-file playback remains unaffected.
- [ ] Commit narrow verification-driven fixes only:

```text
fix: harden YouTube playback lifecycle
```
