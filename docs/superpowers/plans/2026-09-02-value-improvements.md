# Focused player improvements

User scope: five upcoming-game examples (remove anime), last five played sources and reusable neural renders, plus high-value improvements without unnecessary complexity or neural regressions.

Constraints: preserve source resolution, offline sequential neural rendering, full feature-18 verification and atomic cache promotion. Keep the current UI and FFmpeg process lifecycle. No new job system, decoder/timestamp/HDR rewrite, unverified neural sliders, interpolation or durable resume. No commit or publication.

1. Capture the effective neural INI settings, include them in render identity, store the snapshot beside completed renders and reject promotion if settings changed during rendering. Use existing ReShade bootstrap contract, not a new runtime configuration mechanism. Test changed tuning vs unchanged settings.
2. Add a small persistent last-five history file under the owned cache root. Record stable source identity, title, source path, source/render keys. Update on successful playback, reopen cached YouTube sources without resolving signed URLs, and retain at most five histories. Evict only tracked, unreferenced owned cache entries when playback/jobs have released them; never delete user files. Test persistence, deduplication and safe eviction.
3. Add MKV export of the completed cached video with existing source audio, subtitles and chapters, using FFmpeg stream copy. Work asynchronously, support cancellation on close, avoid overwrite and publish only completed output. No burn-in or pixel-format conversion. Test with actual multi-stream fixture.
4. Integrate native File > Recent videos and Export menus. Persist volume, mute, aspect mode, neural preference, SR target and YouTube quality using existing INI machinery. Replace example list with five researched official upcoming-game trailers. Remove ignored legacy quality switches coherently.
5. Build all targets and run CTest, focused real-media export tests, real GPU neural render plus repeat/cache/settings checks. Review integrated changes and document exact verified boundaries.

Implementation uses independent module tasks and root-owned main/menu/CMake integration. No overlapping file ownership or concurrent builds in the same directory.
