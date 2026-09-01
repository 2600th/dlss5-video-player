# Task 6 — YouTube behavior verification

Date: 2026-09-01

## Result

`DONE_WITH_CONCERNS`.

The Release build and all automated gates are green.  A verified production
resolver change was required because the previously selected embedded web
player generated direct URLs that FFprobe received as HTTP 403 for three
fixed Anime examples (and one Game example).  The fixed Android client keeps
the existing one-line HTTPS `*.googlevideo.com` resolver contract and passed
the complete six-example resolver, probe, and short video+audio read matrix.

The only outstanding limitation is in the supplied fixed content: every Anime
example is 56–60 seconds long, so a single continuous two-minute Anime play is
impossible.  Two complete runs of the fixed first Anime example totalled more
than 120 seconds of real FFmpeg/D3D12/DLSS playback.  Native HWND automation
cannot capture audible output, so audio clock/sync is supported by pipeline
and clock evidence, not a human audible observation.

## Changes and TDD evidence

1. `VideoDecoder::ReadNextFFmpegAvailable` could call `StopFFmpeg()` on the
   network-stall read path.  That path synchronously waited up to 500 ms,
   violating the existing `<40 ms` nonblocking-read assertion.  The failure
   reproduced once in three direct `PolicyTests` runs and once under CTest at
   `PolicyTests.cpp:2520`.

   The existing regression test was RED.  The narrow fix retains the normal
   500 ms bounded teardown for close/restart, but calls `StopFFmpeg(0)` only
   when reporting a stall.  Job-object kill-on-close ownership still performs
   cleanup.  Post-fix focused runs and all final CTest runs passed.

2. The helper-replacement test had a real test synchronization race: it used
   `Sleep(75)` before trying to replace `deno.exe`, so under a slow child start
   it sometimes tested before the resolver held its verified helper handle.
   It was reproduced after the focused decoder fix.  The test now waits up to
   500 ms for exactly one additional `yt-dlp.exe` child before attempting the
   replacement.  It continues to assert that replacement is denied.

3. The precise resolver argument test was changed first from
   `youtube:player_client=web_embedded` to `youtube:player_client=android`.
   With production code still using the embedded client, `PolicyTests` failed
   at the exact argument assertion and its fake-resolver child rejected the
   old argument, providing a proper RED state.  The only production change is
   that exact extractor argument.  URL validation, 45-second resolver deadline,
   cancellation, provenance handles, output bounds, and single URL contract
   are unchanged.

## Build and automated gates

The worktree intentionally has no DLSS SDK checkout, so the clean out-of-tree
build was configured against the existing parent checkout without modifying
source dependencies:

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake -S . -B build-task6-clean -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON '-DDLSS_SDK=C:\Users\User\Documents\dlss-5-video-player\external\DLSS'
& $cmake --build build-task6-clean --config Release --clean-first --parallel 4
& $ctest --test-dir build-task6-clean -C Release --output-on-failure
```

Final repeated CTest gate: three consecutive runs, each `2/2` passed:

| Test | Result |
| --- | --- |
| `PolicyTests` | PASS (5.56–5.89 s) |
| `ReleaseApiCompileTests` | PASS (0.01 s) |

## Sanitized resolver / FFmpeg matrix

No direct media URL or query token was printed or stored.  Each row required:
resolver exit 0, exactly one HTTPS Googlevideo URL after the application's
host validation, FFprobe video and audio streams, and a short FFmpeg
`-map 0:v:0 -map 0:a:0 -t 0.5 -f null` read.

| Resolver client | Examples 1–6 probe result | Video+audio read |
| --- | --- | --- |
| `web_embedded` | examples 1 and 3 OK; 2, 4, 5, 6 HTTP 403 | not acceptable |
| `web` | requested MP4 combined format unavailable for all six | not acceptable |
| `android` | all six FFprobe exit 0 | all six FFmpeg exit 0 (116–255 ms) |

This is why Android was selected.  It does not add headers, side channels, or
an alternate URL shape; the existing sanitized `ResolveResult::mediaUrl`
remains the only handoff.

## Actual native-player playback

All launches used `--output 1920x1080 --safe-mode`; no 4K or F6/re-hook stress
was run.  The package-local `DENO_DIR` was set only for each launched process.

| Source | Native command / title | Playback evidence |
| --- | --- | --- |
| Game example 1 | command ID 110, `GTA VI Trailer 2` | 09:44:16–09:46:19, 123 seconds continuous; app HWND responsive; Raw NGX evaluations reached #3600; 1920x1080 output and FFmpeg PCM/WaveOut start logged. |
| Anime example 1 | command ID 113, `2026 Summer Anime Season Trailer` | Source duration 60.1165 seconds. First run began 09:48:58; a Stop-to-zero then Play created a fresh decoder/audio run at 09:50:21 and ran through 09:51:21. Total actual pipeline playback exceeded 120 seconds across two complete source-length runs. |

Renderer logs confirm source dimensions, output dimensions, raw
`CreateFeature SUCCESS`, and repeated `EvaluateFeature_C SUCCESS`.  Audio
uses the FFmpeg PCM/WaveOut path and the player’s audio-master playback design;
without computer-use/audio capture, audible sound and human-perceived sync are
inferred rather than physically observed.

### Native control evidence

Control operations used explicit `WM_COMMAND` messages to the process-owned
player HWND.  The player is custom-painted, not a Win32 toolbar, and its menu
items do not expose a checked state, so `TB_GETSTATE`/checked-menu inspection
is not available.  The evidence retained in the task observation scope is:

- Pause at 09:46:37: no new periodic DLSS evaluation log for 21 seconds.
- Resume at 09:47:05: evaluation resumed (#4200 at 09:47:07).
- Back-10 seek: a fresh FFprobe/FFmpeg video start and PCM audio start were
  logged at 156.7 seconds, followed by completed YouTube media preparation.
- Stop: a fresh zero-second FFprobe/FFmpeg video and PCM audio preparation was
  logged; the player remained responsive.
- Mute on/off, Aspect Fill/Fit, DLSS off/on were sent individually to the
  responsive HWND.  The rendered DLSS-on path is independently proven by the
  raw evaluation log; intermediate mute/aspect/DLSS button state is not
  queryable from the custom UI.
- Fullscreen has direct native state: window rect changed from
  `201,201,1038,760` to `0,0,2194,1234` and back exactly.

## Offline local-file check

For the launched process only, `HTTP_PROXY` and `HTTPS_PROXY` were set to
`http://127.0.0.1:9`; no adapter, firewall, or persistent system setting was
changed.  A local 1280x720 test file still opened and produced FFmpeg video,
PCM/WaveOut audio, and raw DLSS evaluation at 1280x720 output.  This confirms
local playback does not depend on network access.

## Error, process, artifact, and log checks

- `PolicyTests` cover invalid domain and playlist-only URL rejection,
  generic nonzero/private-or-unavailable resolver output, explicit cancellation,
  timeout/output bound behavior, and child/handle cleanup.  The final three
  full CTest runs passed these checks.
- Specifically, `youtube_url_validation_rejects_unsafe_or_unselected_inputs_test`
  drives `IsSupportedYouTubeUrl` through the invalid-domain and playlist-only
  rejection path, while the resolver nonzero-output test verifies the generic
  extraction-error mapping used for unavailable/private input.  Those are
  code-path tests, not a claim that a physical URL dialog was observed for
  each adverse URL.
- A real native cancellation was also exercised: at 09:57:26 an exact player
  HWND received the fixed Game command, observed its `yt-dlp.exe` child, then
  received Stop while resolution was active.  The log recorded `YouTube
  resolution cancelled and worker stopped.`; the child count returned to zero
  within 7 ms and the owner HWND remained responsive.  No helper remained
  after closing the app.
- In an earlier real-app pass the three Anime examples generated responsive,
  generic FFmpeg-open errors under the broken embedded client; no direct URL
  was displayed or logged.  Under the Android fix all three probe/open.
- Every player close recorded no residual `yt-dlp.exe`, `deno.exe`,
  `ffmpeg.exe`, or `ffprobe.exe` child.  The final audit also found zero such
  children.
- `DLSSVideoPlayer.log` and retained task logs had zero matches for
  `googlevideo.com` or direct URL query-token patterns (`sig`, `signature`,
  `token`, `expire`, `ei`, `ip`, `id`, `itag`, `source`).
- User Downloads snapshot count was unchanged: 860 before and after.  No
  player-created MP4/MKV/WebM/media file appeared beside the app, in Downloads,
  or in the task-specific TEMP observation scope.
- Player-created app-local files were logs/settings (`DLSSVideoPlayer.log`,
  `DLSSVideoPlayer.ini`, `ngx_logs`) only.  The Deno helper cache contained
  seven entries exclusively under
  `build-task6-clean\Release\youtube-helper-cache`; that disposable build
  directory is untracked/excluded and is not a release payload.

## Observation artifacts and cleanup

Observation metadata, native window/control records, screenshots, sanitized
matrix results, and copied logs are under:

`%TEMP%\dlss-youtube-task6-observation-20260901`

The task-created `build-task6-clean` directory contains the disposable Release
runtime and helper cache.  Its exact resolved path was verified to be within
this worktree, but the host deletion policy rejected the scoped removal
command.  It is separate from the pre-existing, unrelated untracked
`build-task5-clean` directory, which was not touched.  It should be safely
removed by the owner after evidence review; it is not a source or release
payload change.
