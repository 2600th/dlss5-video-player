# The GPU and audio smokes: every test labelled `gpu` or `audio`, and the
# executables only they run. CMakeLists.txt includes this when
# DLSS_VIDEO_PLAYER_HARDWARE_TESTS is ON, the default. A machine that cannot run
# them - CI's hosted runners, whose device-free run skipped every one - turns it
# OFF and no longer compiles and links a dozen executables it never starts.
# Hardware runs (`ctest --preset hardware`, gpu-tests.yml) need it ON.

dlss_video_player_test(UpscalingGpuSmoke tests/UpscalingGpuSmoke.cpp)
# The DLSS-G admission probe. It links NeuralPreflight for HexResultText, so
# the hex NGX codes this probe prints are produced by the same function the
# receipts use.
dlss_video_player_test(DlssgProbeSmoke tests/DlssgProbeSmoke.cpp)
# The NGX log this probe writes beside its executable is its evidence, and
# Log opens DLSSVideoPlayer.log with ios::trunc, so it gets its own place
# rather than erasing the player's log or having its own erased.
set_target_properties(DlssgProbeSmoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dlssg-probe/$<CONFIG>")
# Measured on 2026-09-17 (RTX 5090, driver 616.64): with no nvngx_dlssg.dll
# beside the executable, CreateFeature(FrameGeneration) answers 0xbad0000b
# and the capability block reads FrameGeneration.Available=0 /
# FeatureInitResult=0xbad00004, even though NGX locates and logs the
# driver-store fallback snippet. With the vendored snippet staged the same
# create returns Success. So the probe stages it the way the player stages
# nvngx_dlss.dll, or it would be measuring a missing file instead of the
# runtime's answer.
add_custom_command(TARGET DlssgProbeSmoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DLSS_SDK}/lib/Windows_x86_64/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlssg.dll"
        "$<TARGET_FILE_DIR:DlssgProbeSmoke>/nvngx_dlssg.dll"
)
# The DLSS-G production experiment: same backend, same snippet staging, same
# include dirs and libraries as the admission probe beside it, because the
# question it asks only means something about the feature the probe admitted.
# It reads an interpolated frame back to the CPU, so it gets its own output
# directory for the same reason the probe does - Log truncates
# DLSSVideoPlayer.log on open and the NGX log beside the executable is the
# evidence.
dlss_video_player_test(DlssgEvaluateSmoke tests/DlssgEvaluateSmoke.cpp)
set_target_properties(DlssgEvaluateSmoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dlssg-evaluate/$<CONFIG>")
add_custom_command(TARGET DlssgEvaluateSmoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DLSS_SDK}/lib/Windows_x86_64/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlssg.dll"
        "$<TARGET_FILE_DIR:DlssgEvaluateSmoke>/nvngx_dlssg.dll"
)
# The frame-generation conversion end to end: decode a real clip, generate
# the intermediates, encode, then probe the result. It asserts the things
# that separate a conversion from a demo - the frame count is the multiple,
# the duration did not move, the audio arrived, and the generated frames
# sit strictly inside their pair at even fractions of it, measured from
# decoded pixels - so it needs FFmpeg staged, the same way CachedExportTests
# does, and now needs it for the measurement itself rather than only for
# the conversion.
# Every combination of the three export stages on one clip. Links the frame
# generation pass directly and reaches the other two through NeuralWorker,
# which is how the player drives them.
dlss_video_player_test(ExportMatrixSmoke tests/ExportMatrixSmoke.cpp)
add_dependencies(ExportMatrixSmoke NeuralWorker)

dlss_video_player_test(FrameGenerationSmoke tests/FrameGenerationSmoke.cpp)
set_target_properties(FrameGenerationSmoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame-generation/$<CONFIG>")
add_custom_command(TARGET FrameGenerationSmoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DLSS_SDK}/lib/Windows_x86_64/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlssg.dll"
        "$<TARGET_FILE_DIR:FrameGenerationSmoke>/nvngx_dlssg.dll"
)
dlss_video_player_test(MediaGpuSmoke tests/MediaGpuSmoke.cpp)
add_dependencies(MediaGpuSmoke NeuralWorker)

# Renders a RANGE through the real helper - the one shape of neural render
# nothing covered, and the one every live session performs. A range render
# begins with preroll frames that are evaluated and never captured, so
# nothing retires their submissions and the add-on's four-generation NR
# workset pool is exhausted before the first captured frame. That is what
# fd9279b broke while all 21 tests stayed green. See the comment at the top
# of the source.
dlss_video_player_test(NeuralRangeRenderSmoke tests/NeuralRangeRenderSmoke.cpp)
add_dependencies(NeuralRangeRenderSmoke NeuralWorker)
# The hardware smokes need an RTX GPU, a display, and (MediaGpuSmoke) the neural
# runtime staged beside NeuralWorker.exe. They are labelled `gpu` so the portable
# suite is `ctest -LE gpu` and the opt-in run on such a machine is `ctest -L gpu`.
add_test(NAME UpscalingGpuSmoke COMMAND UpscalingGpuSmoke
    "${CMAKE_SOURCE_DIR}/docs/media/neural-comparison-demo.mp4" 1440)
# The Super Resolution quality gate (W4-SR): a generated clip upscaled and scored
# with VMAF, failing when a held frame decays - which the flow engine's field on
# identical frames used to do. See RunSrQualityProbe.
add_test(NAME UpscalingSrQualitySmoke COMMAND UpscalingGpuSmoke
    "${FFMPEG_STAGED_DIR}" sr-quality "${CMAKE_BINARY_DIR}/sr-quality")
# The motion and depth views on a paused frame whose guides were never drawn
# named the wrong resource states; this holds the renderer to no debug-layer
# error before and after the first guided frame. Needs no clip.
add_test(NAME DebugViewGpuSmoke COMMAND UpscalingGpuSmoke debug-views)
# RTX VSR (P2.8): the feature created on the player's renderer, evaluated beside
# DLSS Super Resolution in the same frames, every mode presented from it, and the
# quality ladder timed. Only a build with the SDK has an engine to test.
if(RTX_VSR_FOUND)
    add_test(NAME VsrGpuSmoke COMMAND UpscalingGpuSmoke vsr
        "${CMAKE_SOURCE_DIR}/docs/media/neural-comparison-demo.mp4")
    set_tests_properties(VsrGpuSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 300)
    # The export pass (VsrUpscalePass): the clip to 1440p and a one-second range.
    add_test(NAME VsrExportSmoke COMMAND UpscalingGpuSmoke vsr-export
        "${CMAKE_SOURCE_DIR}/docs/media/neural-comparison-demo.mp4" "${CMAKE_BINARY_DIR}/vsr-export")
    set_tests_properties(VsrExportSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 300)
endif()
add_test(NAME MediaGpuSmoke COMMAND MediaGpuSmoke
    "${FFMPEG_STAGED_DIR}" "$<TARGET_FILE:NeuralWorker>" "${CMAKE_BINARY_DIR}/gpu-smoke")
add_test(NAME NeuralRangeRenderSmoke COMMAND NeuralRangeRenderSmoke
    "${FFMPEG_STAGED_DIR}" "$<TARGET_FILE:NeuralWorker>" "${CMAKE_BINARY_DIR}/neural-range-smoke")
# Direct NVENC encoding (P3.7) writes the packets the encoder child writes, which
# is what lets it share the child's cache key. The first proves it on raw frames
# at the encoder, NV12 and P010, with nothing else in the way; the second inside
# real renders through feature 18, one per rung the direct path serves.
add_test(NAME NvencDirectIdentitySmoke COMMAND MediaGpuSmoke --nvenc-direct-identity
    "${FFMPEG_STAGED_DIR}" "${CMAKE_BINARY_DIR}/nvenc-direct-identity")
# A clip stored landscape with a display matrix, stood up by transpose_cuda on
# the CUDA decode path and held byte for byte to ffmpeg's CPU turn.
add_test(NAME TurnedDecodeGpuSmoke COMMAND MediaGpuSmoke --turned-decode
    "${FFMPEG_STAGED_DIR}" "${CMAKE_BINARY_DIR}/turned-decode")
add_test(NAME NeuralDirectEncodeSmoke COMMAND NeuralRangeRenderSmoke
    "${FFMPEG_STAGED_DIR}" "$<TARGET_FILE:NeuralWorker>" "${CMAKE_BINARY_DIR}/neural-direct-encode"
    --direct-encode-identity)
# The preflight probe, which is what the player runs before its first render
# and latches the answer from. Every other neural test renders long enough to
# clear RenoDX 6.x's injection warm-up; this one stops as soon as the probe
# says it is satisfied, which is exactly the path a runtime bump can break
# without any of the others noticing.
add_test(NAME NeuralPreflightSmoke COMMAND NeuralWorkerTests
    --real-preflight "$<TARGET_FILE:NeuralWorker>")
# All seven combinations of the three export stages on one 720p30 clip.
# The stages have separate tests; this is the only thing that proves they
# COMPOSE, and that the upscale actually reaches NGX - the first run of it
# caught the output size being plumbed everywhere except into the feature
# create, which every single-stage test was blind to.
add_test(NAME ExportMatrixSmoke COMMAND ExportMatrixSmoke
    "${FFMPEG_STAGED_DIR}" "$<TARGET_FILE:NeuralWorker>" "${CMAKE_BINARY_DIR}/export-matrix")
add_test(NAME DlssgProbeSmoke COMMAND DlssgProbeSmoke)
add_test(NAME DlssgEvaluateSmoke COMMAND DlssgEvaluateSmoke)
add_test(NAME FrameGenerationSmoke COMMAND FrameGenerationSmoke
    "${CMAKE_SOURCE_DIR}/external/test-media/dlaa-smoke.mp4" 4 "${FFMPEG_STAGED_DIR}"
    "${CMAKE_BINARY_DIR}/frame-generation/frame-generation-smoke.mkv")
# The phase assertions need content whose true motion is known, and
# dlaa-smoke.mp4 is not it: its framing is static, so its brightness
# centroid spans 0.05 px over its first six frames and the harness reports
# its pairs as unmeasurable. This clip is a 200x200 testsrc2 patch on black
# travelling exactly 40 px per source frame, which is the probe the
# corrected phase table was measured with - textured, so the runtime has
# interior detail to localise, rather than a flat blob whose generated
# frames read as a blend of the pair. It is generated at build time instead
# of committed: lossless FFV1 of 60 frames is megabytes of media for a file
# that two ffmpeg filters describe completely.
if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe")
    set(FRAMEGEN_PHASE_CLIP "${CMAKE_BINARY_DIR}/frame-generation/phase-probe-textured.mkv")
    add_custom_command(OUTPUT "${FRAMEGEN_PHASE_CLIP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/frame-generation"
        COMMAND "${FFMPEG_STAGED_DIR}/ffmpeg.exe" -hide_banner -nostdin -loglevel error -y
            -f lavfi -i color=black:s=1280x720:r=30:d=2
            -f lavfi -i testsrc2=s=200x200:r=30:d=2
            -filter_complex [0][1]overlay=x=100+40*n:y=260
            -c:v ffv1 -pix_fmt yuv420p "${FRAMEGEN_PHASE_CLIP}"
        COMMENT "Generating the frame-generation phase probe clip"
        VERBATIM)
    add_custom_target(FrameGenerationPhaseClip DEPENDS "${FRAMEGEN_PHASE_CLIP}")
    add_dependencies(FrameGenerationSmoke FrameGenerationPhaseClip)
    # Multiplier 4 here as well, so the two registrations differ only in the
    # content: 30 fps at 4x is 120 fps, which is what the policy plans for a
    # 30 fps source on this 120 Hz panel.
    add_test(NAME FrameGenerationSmokeTextured COMMAND FrameGenerationSmoke
        "${FRAMEGEN_PHASE_CLIP}" 4 "${FFMPEG_STAGED_DIR}"
        "${CMAKE_BINARY_DIR}/frame-generation/frame-generation-smoke-textured.mkv")
    set_tests_properties(FrameGenerationSmokeTextured PROPERTIES LABELS gpu TIMEOUT 900)
    # A clip with ONE hard cut in it, which is what the scene-cut criterion
    # in src/SceneCut.h exists for: generating across an edit blends two
    # unrelated shots into every slot between them, and it is the most
    # visible thing this pass can get wrong. Two shots chosen so the edit is
    # unambiguous on the criterion's own terms - a textured pattern against
    # a flat white field shares almost no luma distribution - and so the
    # count is exactly one, which the harness asserts. The first shot also
    # moves, so the frames WITHIN it must keep interpolating: a detector
    # that fired on motion would report more than one cut and fail here.
    set(FRAMEGEN_CUT_CLIP "${CMAKE_BINARY_DIR}/frame-generation/scene-cut-probe.mkv")
    add_custom_command(OUTPUT "${FRAMEGEN_CUT_CLIP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/frame-generation"
        COMMAND "${FFMPEG_STAGED_DIR}/ffmpeg.exe" -hide_banner -nostdin -loglevel error -y
            -f lavfi -i testsrc2=s=640x360:r=30:d=1
            -f lavfi -i color=white:s=640x360:r=30:d=1
            -f lavfi -i anoisesrc=d=2:c=pink:r=48000:a=0.1
            -filter_complex "[0:v][1:v]concat=n=2:v=1:a=0[v]"
            -map "[v]" -map 2:a -c:v ffv1 -pix_fmt yuv420p -c:a aac "${FRAMEGEN_CUT_CLIP}"
        COMMENT "Generating the frame-generation scene-cut probe clip"
        VERBATIM)
    add_custom_target(FrameGenerationCutClip DEPENDS "${FRAMEGEN_CUT_CLIP}")
    add_dependencies(FrameGenerationSmoke FrameGenerationCutClip)
    # 2x, so one refused pair is one held frame and the accounting identity
    # the harness checks has the smallest possible arithmetic between it and
    # the defect. The trailing 1 is the cut this clip is known to contain.
    add_test(NAME FrameGenerationSmokeSceneCut COMMAND FrameGenerationSmoke
        "${FRAMEGEN_CUT_CLIP}" 2 "${FFMPEG_STAGED_DIR}"
        "${CMAKE_BINARY_DIR}/frame-generation/frame-generation-smoke-scene-cut.mkv"
        "" 1)
    set_tests_properties(FrameGenerationSmokeSceneCut PROPERTIES LABELS gpu TIMEOUT 900)
    # The stream-source case, in the exact shape of the defect
    # FrameGenerationRequest::streamSource exists for. The player converts
    # the NEURAL RENDER when that view is on screen and this project writes
    # those carriers video-only (MediaPipeline's BuildEncoderArguments
    # passes `-an`), so the converted file's streams can only be copied
    # from the original the carrier was rendered from. That needs two
    # files, both generated here for the same reason the phase clip is -
    # lossless media does not belong in the repository: a VIDEO-ONLY
    # carrier, which is what the pass is handed, and an audio-bearing
    # ORIGINAL of the same geometry, rate and length, which is what the
    # streams must come from. The carrier repeats the phase clip's moving
    # textured patch so the phase assertions stay live on this run too
    # rather than being skipped as a still image.
    set(FRAMEGEN_CARRIER_CLIP "${CMAKE_BINARY_DIR}/frame-generation/stream-source-carrier.mkv")
    set(FRAMEGEN_ORIGINAL_CLIP "${CMAKE_BINARY_DIR}/frame-generation/stream-source-original.mp4")
    add_custom_command(OUTPUT "${FRAMEGEN_CARRIER_CLIP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/frame-generation"
        COMMAND "${FFMPEG_STAGED_DIR}/ffmpeg.exe" -hide_banner -nostdin -loglevel error -y
            -f lavfi -i color=black:s=1280x720:r=30:d=2
            -f lavfi -i testsrc2=s=200x200:r=30:d=2
            -filter_complex [0][1]overlay=x=100+40*n:y=260
            -an -c:v ffv1 -pix_fmt yuv420p "${FRAMEGEN_CARRIER_CLIP}"
        COMMENT "Generating the video-only neural carrier clip"
        VERBATIM)
    add_custom_command(OUTPUT "${FRAMEGEN_ORIGINAL_CLIP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/frame-generation"
        COMMAND "${FFMPEG_STAGED_DIR}/ffmpeg.exe" -hide_banner -nostdin -loglevel error -y
            -f lavfi -i color=black:s=1280x720:r=30:d=2
            -f lavfi -i testsrc2=s=200x200:r=30:d=2
            -f lavfi -i sine=frequency=440:duration=2
            -filter_complex "[0][1]overlay=x=100+40*n:y=260[v]"
            -map [v] -map 2:a:0 -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest
            "${FRAMEGEN_ORIGINAL_CLIP}"
        COMMENT "Generating the audio-bearing original the carrier stands for"
        VERBATIM)
    add_custom_target(FrameGenerationStreamSourceClips
        DEPENDS "${FRAMEGEN_CARRIER_CLIP}" "${FRAMEGEN_ORIGINAL_CLIP}")
    add_dependencies(FrameGenerationSmoke FrameGenerationStreamSourceClips)
    # Multiplier 2, the smallest conversion that still inserts a frame, so
    # this registration costs the least of the three while asking its own
    # question: the frames come from the video-only carrier and the audio
    # has to arrive from the fifth argument. With streamSource unused the
    # mux copies the streams of the video-only carrier, which has none, and
    # the run reports outputAudioStreams=0 - which is what this
    # registration catches. Measured here on exactly these two inputs, at
    # 2x, on an RTX 5090: with the original as the fifth argument
    # sourceAudioStreams=1, outputAudioStreams=1, resultAudioCarried=true,
    # verdict=PASS, exit 0; with the fifth argument pointed at the CARRIER
    # instead - which is what `request.streamSource.empty() ?
    # request.source : request.streamSource` evaluates to when the field is
    # ignored - sourceAudioStreams=0, outputAudioStreams=0,
    # resultAudioCarried=false, verdict=FAIL, exit 1. That negative control
    # has to NAME the carrier rather than omit the argument, because with
    # no fifth argument the harness synthesises a tone onto the silent
    # frame source (sourceAudioSynthesized=true) and the run passes with
    # outputAudioStreams=1 - the pre-existing stage that keeps the other
    # two registrations' audio assertion non-vacuous.
    # The two FAIL patterns below state the defect's signature in the
    # registration rather than leaving it to the exit code alone; they
    # cannot match resultOutputAudioStreams=0, which differs in case.
    add_test(NAME FrameGenerationSmokeStreamSource COMMAND FrameGenerationSmoke
        "${FRAMEGEN_CARRIER_CLIP}" 2 "${FFMPEG_STAGED_DIR}"
        "${CMAKE_BINARY_DIR}/frame-generation/frame-generation-smoke-stream-source.mkv"
        "${FRAMEGEN_ORIGINAL_CLIP}")
    set_tests_properties(FrameGenerationSmokeStreamSource PROPERTIES LABELS gpu TIMEOUT 900
        FAIL_REGULAR_EXPRESSION "outputAudioStreams=0;resultAudioCarried=false")
endif()
# 125, not 2. Every one of these already returns 2 for an ordinary failure -
# bad arguments, a missing ffmpeg.exe, unreadable media - so mapping 2 to
# "skipped" would hide the regressions a GPU runner exists to catch. Each
# smoke opens with gpu_test_gate::SkipWithoutGpu(), so `ctest -L gpu` on a
# machine with no adapter skips rather than reporting five hard failures.
set_tests_properties(UpscalingGpuSmoke UpscalingSrQualitySmoke DebugViewGpuSmoke MediaGpuSmoke DlssgProbeSmoke DlssgEvaluateSmoke
    FrameGenerationSmoke FrameGenerationSmokeTextured FrameGenerationSmokeSceneCut
    FrameGenerationSmokeStreamSource NeuralRangeRenderSmoke NvencDirectIdentitySmoke NeuralDirectEncodeSmoke
    TurnedDecodeGpuSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125)
# Seconds on an RTX 5090 - the trigger is the preroll, not the pixel count,
# so this stays at 640x360. The ceiling is for a slower card, not for a
# hang; a hang is what the timeout is meant to catch.
set_tests_properties(NeuralRangeRenderSmoke PROPERTIES TIMEOUT 600)
# Two encodes of 60 720p frames each way, then four 2 s renders: under a minute.
set_tests_properties(NvencDirectIdentitySmoke NeuralDirectEncodeSmoke PROPERTIES TIMEOUT 600)
# A/V sync had no coverage at all: the audio clock every frame's due time
# is computed from carried one assertion, that a number handed to Seek was
# stored. This needs a real render endpoint, so it is labelled `audio` and
# skips (125) where there is none.
dlss_video_player_test(AudioClockSmoke tests/AudioClockSmoke.cpp)
set_target_properties(AudioClockSmoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-clock/$<CONFIG>")
add_test(NAME AudioClockSmoke COMMAND AudioClockSmoke
    "${FFMPEG_STAGED_DIR}" "${CMAKE_BINARY_DIR}/audio-clock")
set_tests_properties(AudioClockSmoke PROPERTIES LABELS audio SKIP_RETURN_CODE 125 TIMEOUT 300)
# tools/verification/fade-probe.cpp is run by hand against a loopback
# recording (its header says how), so nothing compiled it, and it stopped
# compiling when WasapiRenderer::Write began returning a WriteResult. Built
# here, not registered as a test: an API change now breaks the build instead.
dlss_video_player_test(FadeProbe tools/verification/fade-probe.cpp)

# The same binary, its `--gpu` case set. Nothing in this suite opened a
# network source, and the prepared-renderer commit path is reached by
# nothing else - four defects lived there at once while 23 tests stayed
# green. A second target would compile main.cpp a second time, which is the
# amplification 2.12 is about, so this registers the existing one again.
add_test(NAME NetworkPreparedRendererSmoke COMMAND PlayerUiRegressionTests
    --gpu "${FFMPEG_STAGED_DIR}" "${CMAKE_BINARY_DIR}/network-prepared")
set_tests_properties(NetworkPreparedRendererSmoke PROPERTIES
    LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 300)

# DlssgProbeSmoke creates one feature and no frames, so it belongs with the
# 300 s budgets rather than with the two smokes that render.
set_tests_properties(DlssgProbeSmoke PROPERTIES TIMEOUT 300)
set_tests_properties(NeuralPreflightSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 300)
# Eight decodes of a one-second 320x176 clip.
set_tests_properties(TurnedDecodeGpuSmoke PROPERTIES TIMEOUT 300)
# Seven renders of a 3.5 s clip, two of them at 1440p. Measured at 96 s.
set_tests_properties(ExportMatrixSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 900)
# FrameGenerationSmoke decodes, generates and encodes a whole clip, so it
# belongs with the two smokes that render rather than with the 300 s probes.
set_tests_properties(UpscalingGpuSmoke UpscalingSrQualitySmoke MediaGpuSmoke FrameGenerationSmoke PROPERTIES TIMEOUT 900)
