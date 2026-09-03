# Media, portable cache and fullscreen verification

Local source changes tested on 2026-09-03; these additions have not been published
as a GitHub release. Hardware: NVIDIA GeForce RTX 5090, driver 610.62.

## Final build and package gate

The final clean Windows Release build succeeded. All nine CTest suites passed
in 39.63 seconds, including real media exports, native UI, cache migration and
sparse-runtime-receipt regressions. Both the staged package and extracted ZIP
passed the 42-file allowlist/manifest verifier. The packaged player and neural
worker SHA-256 values match the final tested build binaries.

Local package: `dist/DLSSVideoPlayer-v0.14.0-win64.zip`
(318,458,601 bytes).

SHA-256: `5D6BCD2932E18BFFDC19EFB3F3BFED84907CEF2E62DE10FB6C24FC64224A2502`.

## Real processing and export

The opt-in `MediaGpuSmoke` executable generated deterministic photo, animated GIF
and video inputs, invoked the actual isolated neural worker, and exported each
validated result to PNG, JPEG, GIF, MP4 and MKV. All 15 exports passed decoding,
dimensions and applicable frame-count/duration checks. Animated GIF and video
outputs contained changing pixels; photo caches and image exports retained one frame.

| Input | Source size | Captured frames | Video extent | Neural job time |
| --- | --- | --- | --- | --- |
| PNG photo | 640 x 360 | 1 | 1 second carrier | 6.96 seconds |
| Variable-delay GIF | 320 x 180 | 100 | 1 second | 7.33 seconds |
| H.264/AAC video | 640 x 360 | 24 | 1 second | 6.83 seconds |

Each job returned valid armed feature-18 evidence and a fresh runtime receipt
at evaluation 60. GIF exports contained 50 encoded frames over one second.
The MKV video retained audio, with a 0.999-second decoded video extent and
1.023-second container duration. These timing differences remain within one
source frame. Synthetic test patterns were visually intact; this verifies
processing and file integrity, not photorealistic quality across arbitrary media.

The first hardware run correctly rejected photos and short video because the
runtime emits sparse evaluation receipts. The fix captures the first source
frame until a new receipt is observed, retains the latest capture once, and
keeps strict final validation. It never extends the exported timeline.

Evidence and all output files are retained locally under
`build-upscaling/media-verification/run-02/`, including `results.txt` and each
input's ReShade/player logs. Reproduce in a new output directory:

```powershell
.\build-upscaling\Release\MediaGpuSmoke.exe .\external\ffmpeg\bin `
  .\build-upscaling\Release\neural-runtime\NeuralWorker.exe `
  .\build-upscaling\media-verification\new-run
```

## Regression coverage

Real FFmpeg tests cover PNG/JPEG/BMP/TIFF/static WebP loading, one-frame photo
encoding, JPEG EXIF rotation, odd source dimensions, GIF final-frame holds and
motion, MP4 audio/subtitle languages and chapters, and MKV stream-copy hashes.
Cancellation, failed encodes, existing destinations and competing writers are
covered by the shared atomic export tests.

Native Win32 regressions exercise fullscreen entry, content bounds, menu/toolbar
visibility, mouse movement in the renderer and letterbox, idle expiry, menu and
drag guards, keyboard access, and restoring window geometry. They do not claim
a manual interactive desktop visual review.

Portable cache tests run copied executables from another working directory and
cover writable adjacent storage, blocked directory/layout fallback, explicit
custom roots, and automatic-setting migration/relocation. Existing source,
cache and exported files are preserved.

Playback DLSS Super Resolution and display color adjustments remain display-only;
exports contain the offline neural result at source resolution. GIF export uses
20 ms frame intervals; transparent/HDR/camera RAW workflows were not verified.
