cmake_minimum_required(VERSION 3.24)

# VERSION is the single source of the release number: the release workflow gates
# the tag against it, the assembler compares the built executable's version
# resource against it, and resources.rc reads it through generated/VersionData.h.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DLSS_VIDEO_PLAYER_VERSION_RAW)
string(STRIP "${DLSS_VIDEO_PLAYER_VERSION_RAW}" DLSS_VIDEO_PLAYER_VERSION_FILE)
if(NOT DLSS_VIDEO_PLAYER_VERSION_FILE MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must read major.minor.patch; received '${DLSS_VIDEO_PLAYER_VERSION_FILE}'.")
endif()
project(DLSSVideoPlayer VERSION ${DLSS_VIDEO_PLAYER_VERSION_FILE} LANGUAGES CXX RC)

include(CTest)

if(NOT WIN32)
    message(FATAL_ERROR "DLSSVideoPlayer currently targets Windows x64 / D3D12 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "This build needs MSVC: Visual Studio 2022 or newer, or its Build Tools.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# A single-config generator with no CMAKE_BUILD_TYPE compiles with no /O2, no
# /Ob2 and no NDEBUG - asserts live, every inline call real - and
# `--build --config Release` is silently ignored there, so nothing says so.
# build_windows.bat has a branch that passes no generator at all, which is
# taken whenever cmake came from PATH, so this is reachable from the
# documented build. A project that publishes measured numbers cannot have a
# path that quietly produces an unoptimized binary.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    message(STATUS "No CMAKE_BUILD_TYPE on a single-config generator; defaulting to Release.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type for single-config generators" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Source files are UTF-8 without a BOM. Do not let the Windows ANSI code page
# reinterpret UI punctuation when compiling wide string literals.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

# Compile a target's translation units in parallel. MSBuild's /m already runs
# projects side by side; this is the other axis, and it matters here because
# the same sources are compiled into twenty executables - 162 object files for
# 31 sources - so the long pole is one project with many TUs, not many
# projects.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/MP>)

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Provide the pinned official NVIDIA/DLSS checkout documented in docs/BUILDING.md.")
endif()

set(DLSS_LIB_DIR "${DLSS_SDK}/lib/Windows_x86_64/x64")
# NGX ships one import library per CRT: nvsdk_ngx_d.lib is built against the release
# CRT and nvsdk_ngx_d_dbg.lib against the debug CRT at _ITERATOR_DEBUG_LEVEL 2, which
# is what CMAKE_MSVC_RUNTIME_LIBRARY above gives a Debug build. Linking the release
# one there fails with LNK2038 on every NGX target, so the choice is made once here
# and every target that names NGX links this variable.
set(DLSS_NGX_LIBRARY "$<IF:$<CONFIG:Debug>,nvsdk_ngx_d_dbg,nvsdk_ngx_d>")
set(DLSS_RUNTIME_REL "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
set(DLSS_RUNTIME_DEV "${DLSS_SDK}/lib/Windows_x86_64/dev/nvngx_dlss.dll")
# The Frame Generation snippet from the same pinned SDK. NGX refuses feature 15
# with 0xbad0000b unless this sits beside the executable, even though it locates
# and logs the driver-store fallback - measured both ways by DlssgProbeSmoke.
set(DLSSG_RUNTIME_REL "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlssg.dll")
set(DLSSG_RUNTIME_DEV "${DLSS_SDK}/lib/Windows_x86_64/dev/nvngx_dlssg.dll")

# The two Optical Flow interface headers live in external/nvof: NVIDIA licenses each
# under MIT in its own copyright block, so unlike the DLSS SDK they are committed and
# there is nothing to fetch. Absent, the build still works and the CPU estimator in
# TemporalGuides.cpp is the only motion backend, so a fork that strips them is not
# broken - just slower and coarser.
set(NVOF_SDK "${CMAKE_SOURCE_DIR}/external/nvof" CACHE PATH "Path to the NVIDIA Optical Flow SDK NvOFInterface headers")
if(EXISTS "${NVOF_SDK}/nvOpticalFlowD3D12.h")
    set(NVOF_FOUND TRUE)
    message(STATUS "NVIDIA Optical Flow SDK headers found at ${NVOF_SDK}; hardware optical flow enabled.")
else()
    set(NVOF_FOUND FALSE)
    message(WARNING "NVIDIA Optical Flow SDK headers are missing from ${NVOF_SDK}. They ship with this repository, so this build will fall back to the CPU motion estimator - see docs/BUILDING.md.")
endif()

# Every target that compiles D3D12Renderer.cpp also compiles the flow backend; this
# only points it at the headers, because the source is listed with the renderer.
function(dlss_video_player_use_nvof target)
    if(NVOF_FOUND)
        target_include_directories(${target} PRIVATE "${NVOF_SDK}")
        target_compile_definitions(${target} PRIVATE DLSS_VIDEO_PLAYER_HAS_NVOF)
    endif()
endfunction()

# Control Flow Guard and CET shadow stacks for the two shipped binaries. The
# compiler and linker already default to /GS, /DYNAMICBASE, /NXCOMPAT and
# /HIGHENTROPYVA; they are spelled out so a toolset that stops defaulting them
# shows up as a diff here rather than in dumpbin output of a shipped package.
function(dlss_video_player_harden target)
    target_compile_options(${target} PRIVATE /guard:cf /GS)
    target_link_options(${target} PRIVATE /GUARD:CF /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
endfunction()

# The packaging lock is embedded so the player and helper verify the exact
# runtime files at run time, not only at packaging time.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/runtime-lock.json")
file(READ "${CMAKE_SOURCE_DIR}/packaging/runtime-lock.json" RUNTIME_LOCK_JSON)
configure_file(src/RuntimeLockData.h.in "${CMAKE_BINARY_DIR}/generated/RuntimeLockData.h" @ONLY)
configure_file(src/VersionData.h.in "${CMAKE_BINARY_DIR}/generated/VersionData.h" @ONLY)
set(GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")

add_executable(DLSSVideoPlayer WIN32
    src/main.cpp
    src/resources.h
    src/resources.rc
    src/VersionData.h.in
    src/RuntimeLockData.h.in
    src/AppMenu.cpp
    src/AppMenu.h
    src/UiLayout.cpp
    src/UiLayout.h
    src/UiResources.cpp
    src/UiResources.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/MediaSource.cpp
    src/DLSSBackend.cpp
    src/DLSSBackend.h
    src/DLSSGBackend.cpp
    src/DLSSGBackend.h
    src/FrameGenerationPass.cpp
    src/FrameGenerationPass.h
    src/FrameRatePolicy.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/OpticalFlowNvof.cpp
    src/OpticalFlowNvof.h
    src/FlowGate.h
    src/NvofResolveShader.h
    src/HardErrorSuppression.h
    src/D3D12FenceWait.h
    src/TemporalGuides.cpp
    src/TemporalGuides.h
    src/PixelLayout.h
    src/ParallelFor.h
    src/AudioPlayer.cpp src/WasapiRenderer.cpp
    src/AudioPlayer.h
    src/Log.h
    src/Localization.h
    # Exported driver hints; the comment in GpuPreference.cpp explains why they
    # are a translation unit of their own and why both binaries compile it.
    src/GpuPreference.cpp
    src/GpuPreference.h
    src/RuntimePolicy.cpp
    src/RuntimePolicy.h
    src/ReShadeConfig.cpp
    src/ReShadeConfig.h
    src/YouTubeResolver.cpp
    src/YouTubeResolver.h
    src/ExampleVideos.h
    src/RecentMedia.cpp
    src/RecentMedia.h
    src/CompletionRegistry.h
    src/NetworkMediaTransaction.h
    src/NeuralCache.cpp
    src/NeuralCache.h
    src/MediaPipeline.cpp
    src/MediaPipeline.h
    src/OfflineNeuralRenderer.cpp
    src/NeuralRenderTypes.h
    src/OfflineNeuralRenderer.h
    src/NeuralWorker.cpp
    src/NeuralWorker.h
    src/ResidentHelperPolicy.h
    src/SynchronizedPlayback.cpp
    src/SynchronizedPlayback.h
    src/NeuralSegmentIndex.h
    src/NeuralCoverage.h
    src/LiveSessionPolicy.h
    src/PlaybackTiming.h
    src/UpscalingPolicy.h
    src/NgxSession.h
    src/RuntimeLifetime.h
    src/FrameIdentity.h
    src/GuideControls.h
    src/NeuralWorkerProtocol.h
    src/NeuralPreflight.cpp
    src/NeuralPreflight.h
    src/RuntimeLock.cpp
    src/RuntimeLock.h
    src/NeuralReceipt.cpp
    src/NeuralReceipt.h
    src/NeuralSettings.cpp
    src/NeuralSettings.h
    src/RangeSelection.cpp
    src/RangeSelection.h
    src/UpdateCheck.cpp
    src/UpdateCheck.h
)

target_compile_definitions(DLSSVideoPlayer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}"
)
target_compile_options(DLSSVideoPlayer PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(DLSSVideoPlayer PRIVATE "${DLSS_SDK}/include" "${GENERATED_INCLUDE_DIR}")
target_link_directories(DLSSVideoPlayer PRIVATE "${DLSS_LIB_DIR}")
target_link_libraries(DLSSVideoPlayer PRIVATE
    d3d12 dxgi dxguid d3dcompiler
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi winhttp bcrypt dbghelp
    ${DLSS_NGX_LIBRARY}
)
dlss_video_player_use_nvof(DLSSVideoPlayer)
dlss_video_player_harden(DLSSVideoPlayer)

# Only the offline helper loads the experimental proxy/add-on. The player root
# stays hook-free for its independent runtime Super Resolution feature.
add_executable(NeuralWorker src/NeuralWorkerMain.cpp src/NeuralWorker.cpp
    src/NeuralPreflight.cpp src/NeuralPreflightProbe.cpp src/NeuralCache.cpp src/RuntimeLock.cpp
    src/OfflineNeuralRenderer.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/MediaSource.cpp
    src/D3D12Renderer.cpp src/DLSSBackend.cpp src/TemporalGuides.cpp src/OpticalFlowNvof.cpp
    src/RuntimePolicy.cpp src/ReShadeConfig.cpp src/GpuPreference.cpp)
target_compile_definitions(NeuralWorker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}")
target_include_directories(NeuralWorker PRIVATE "${DLSS_SDK}/include" "${GENERATED_INCLUDE_DIR}")
target_compile_options(NeuralWorker PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_directories(NeuralWorker PRIVATE "${DLSS_LIB_DIR}")
get_target_property(player_libraries DLSSVideoPlayer LINK_LIBRARIES)
target_link_libraries(NeuralWorker PRIVATE ${player_libraries})
dlss_video_player_use_nvof(NeuralWorker)
dlss_video_player_harden(NeuralWorker)
set_target_properties(NeuralWorker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/neural-runtime")
add_dependencies(DLSSVideoPlayer NeuralWorker)

add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${DLSS_RUNTIME_DEV},${DLSS_RUNTIME_REL}>"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/nvngx_dlss.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${DLSSG_RUNTIME_DEV},${DLSSG_RUNTIME_REL}>"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/nvngx_dlssg.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/README.md"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/README.md"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/LICENSE"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/LICENSE"
)

# The one-click BAT stages these binaries automatically. Keep this optional so
# developers can still configure CMake before downloading FFmpeg.
set(FFMPEG_STAGED_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin" CACHE PATH "Path to the verified FFmpeg bin directory")
if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe" AND EXISTS "${FFMPEG_STAGED_DIR}/ffprobe.exe")
    add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffmpeg.exe"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/ffmpeg.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_STAGED_DIR}/ffprobe.exe"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/ffprobe.exe"
    )
endif()

set(YOUTUBE_STAGED_DIR "${CMAKE_SOURCE_DIR}/external/youtube" CACHE PATH "Path to the pinned yt-dlp and Deno executables")
if(EXISTS "${YOUTUBE_STAGED_DIR}/yt-dlp.exe" AND EXISTS "${YOUTUBE_STAGED_DIR}/deno.exe")
    add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${YOUTUBE_STAGED_DIR}/yt-dlp.exe"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/yt-dlp.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${YOUTUBE_STAGED_DIR}/deno.exe"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/deno.exe"
    )
endif()

if(BUILD_TESTING)
    add_executable(RecentMediaTests tests/RecentMediaTests.cpp src/RecentMedia.cpp)
    add_executable(RenderSettingsTests tests/RenderSettingsTests.cpp src/ReShadeConfig.cpp src/NeuralCache.cpp
        src/NeuralSettings.cpp)
    # CachedExportTests drives SynchronizedPlayback through its owning constructor,
    # which is the only place the decoder-backed frame sources are built, and feeds a
    # real join's probe into the publish gate in RuntimePolicy.
    add_executable(CachedExportTests tests/CachedExportTests.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp
        src/MediaSource.cpp src/SynchronizedPlayback.cpp src/RuntimePolicy.cpp)
    add_executable(RuntimeLockTests tests/RuntimeLockTests.cpp src/RuntimeLock.cpp src/NeuralReceipt.cpp
        src/NeuralPreflight.cpp src/NeuralCache.cpp)
    add_executable(RangeSelectionTests tests/RangeSelectionTests.cpp src/RangeSelection.cpp)
    add_executable(FrameIdentityTests tests/FrameIdentityTests.cpp src/TemporalGuides.cpp
        src/SynchronizedPlayback.cpp src/VideoDecoder.cpp src/MediaSource.cpp)
    foreach(test_target RecentMediaTests RenderSettingsTests CachedExportTests RuntimeLockTests
            RangeSelectionTests FrameIdentityTests)
        target_include_directories(${test_target} PRIVATE src "${GENERATED_INCLUDE_DIR}")
        target_compile_definitions(${test_target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(${test_target} PRIVATE /W4 /permissive- /EHsc)
    endforeach()
    target_compile_definitions(RuntimeLockTests PRIVATE DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}")
    target_link_libraries(RecentMediaTests PRIVATE user32)
    target_link_libraries(RenderSettingsTests PRIVATE bcrypt shell32 ole32)
    target_link_libraries(CachedExportTests PRIVATE shell32 mfplat mfreadwrite mfuuid ole32 dxgi dxguid)
    target_link_libraries(RuntimeLockTests PRIVATE bcrypt shell32 ole32 version)
    target_link_libraries(FrameIdentityTests PRIVATE mfplat mfreadwrite mfuuid ole32 shell32)
    add_test(NAME RecentMediaTests COMMAND RecentMediaTests)
    add_test(NAME RenderSettingsTests COMMAND RenderSettingsTests)
    add_test(NAME CachedExportTests COMMAND CachedExportTests "${FFMPEG_STAGED_DIR}")
    # FFmpeg is optional at configure time (FFMPEG_STAGED_DIR above), so a tree
    # without it reports this real-media suite as skipped rather than failed.
    set_tests_properties(CachedExportTests PROPERTIES SKIP_RETURN_CODE 125)
    add_test(NAME RuntimeLockTests COMMAND RuntimeLockTests)
    add_test(NAME RangeSelectionTests COMMAND RangeSelectionTests)
    add_test(NAME FrameIdentityTests COMMAND FrameIdentityTests)
    add_executable(UpscalingGpuSmoke tests/UpscalingGpuSmoke.cpp
        src/D3D12Renderer.cpp src/DLSSBackend.cpp src/TemporalGuides.cpp src/OpticalFlowNvof.cpp
        src/VideoDecoder.cpp src/MediaSource.cpp src/RuntimePolicy.cpp)
    target_include_directories(UpscalingGpuSmoke PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(UpscalingGpuSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(UpscalingGpuSmoke PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_directories(UpscalingGpuSmoke PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(UpscalingGpuSmoke PRIVATE ${player_libraries})
    dlss_video_player_use_nvof(UpscalingGpuSmoke)
    # The DLSS-G admission probe. NeuralPreflight.cpp (with the NeuralCache.cpp it
    # needs) is compiled in for HexResultText, so the hex NGX codes this probe
    # prints are produced by the same function the receipts use.
    add_executable(DlssgProbeSmoke tests/DlssgProbeSmoke.cpp
        src/DLSSGBackend.cpp src/NeuralPreflight.cpp src/NeuralCache.cpp)
    target_include_directories(DlssgProbeSmoke PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(DlssgProbeSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DlssgProbeSmoke PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_directories(DlssgProbeSmoke PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(DlssgProbeSmoke PRIVATE ${player_libraries})
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
    add_executable(DlssgEvaluateSmoke tests/DlssgEvaluateSmoke.cpp
        src/DLSSGBackend.cpp src/NeuralPreflight.cpp src/NeuralCache.cpp)
    target_include_directories(DlssgEvaluateSmoke PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(DlssgEvaluateSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(DlssgEvaluateSmoke PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_directories(DlssgEvaluateSmoke PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(DlssgEvaluateSmoke PRIVATE ${player_libraries})
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
    add_executable(FrameGenerationSmoke tests/FrameGenerationSmoke.cpp
        src/FrameGenerationPass.cpp src/DLSSGBackend.cpp src/NeuralPreflight.cpp src/NeuralCache.cpp
        src/MediaPipeline.cpp src/VideoDecoder.cpp src/MediaSource.cpp)
    target_include_directories(FrameGenerationSmoke PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(FrameGenerationSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(FrameGenerationSmoke PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_directories(FrameGenerationSmoke PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(FrameGenerationSmoke PRIVATE ${player_libraries})
    set_target_properties(FrameGenerationSmoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/frame-generation/$<CONFIG>")
    add_custom_command(TARGET FrameGenerationSmoke POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLSS_SDK}/lib/Windows_x86_64/$<IF:$<CONFIG:Debug>,dev,rel>/nvngx_dlssg.dll"
            "$<TARGET_FILE_DIR:FrameGenerationSmoke>/nvngx_dlssg.dll"
    )
    add_executable(MediaGpuSmoke tests/MediaGpuSmoke.cpp
        src/NeuralWorker.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/MediaSource.cpp)
    target_include_directories(MediaGpuSmoke PRIVATE src)
    target_compile_definitions(MediaGpuSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(MediaGpuSmoke PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(MediaGpuSmoke PRIVATE shell32 mfplat mfreadwrite mfuuid ole32 dxgi)
    add_dependencies(MediaGpuSmoke NeuralWorker)

    # Renders a RANGE through the real helper - the one shape of neural render
    # nothing covered, and the one every live session performs. A range render
    # begins with preroll frames that are evaluated and never captured, so
    # nothing retires their submissions and the add-on's four-generation NR
    # workset pool is exhausted before the first captured frame. That is what
    # fd9279b broke while all 21 tests stayed green. See the comment at the top
    # of the source.
    add_executable(NeuralRangeRenderSmoke tests/NeuralRangeRenderSmoke.cpp
        src/NeuralWorker.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/MediaSource.cpp)
    target_include_directories(NeuralRangeRenderSmoke PRIVATE src)
    target_compile_definitions(NeuralRangeRenderSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(NeuralRangeRenderSmoke PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(NeuralRangeRenderSmoke PRIVATE shell32 mfplat mfreadwrite mfuuid ole32 dxgi)
    add_dependencies(NeuralRangeRenderSmoke NeuralWorker)
    # The hardware smokes need an RTX GPU, a display, and (MediaGpuSmoke) the neural
    # runtime staged beside NeuralWorker.exe. They are labelled `gpu` so the portable
    # suite is `ctest -LE gpu` and the opt-in run on such a machine is `ctest -L gpu`.
    add_test(NAME UpscalingGpuSmoke COMMAND UpscalingGpuSmoke
        "${CMAKE_SOURCE_DIR}/docs/media/neural-comparison-demo.mp4" 1440)
    add_test(NAME MediaGpuSmoke COMMAND MediaGpuSmoke
        "${FFMPEG_STAGED_DIR}" "$<TARGET_FILE:NeuralWorker>" "${CMAKE_BINARY_DIR}/gpu-smoke")
    add_test(NAME NeuralRangeRenderSmoke COMMAND NeuralRangeRenderSmoke
        "${FFMPEG_STAGED_DIR}" "$<TARGET_FILE:NeuralWorker>" "${CMAKE_BINARY_DIR}/neural-range-smoke")
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
    set_tests_properties(UpscalingGpuSmoke MediaGpuSmoke DlssgProbeSmoke DlssgEvaluateSmoke
        FrameGenerationSmoke FrameGenerationSmokeTextured FrameGenerationSmokeSceneCut
        FrameGenerationSmokeStreamSource NeuralRangeRenderSmoke
        PROPERTIES LABELS gpu SKIP_RETURN_CODE 125)
    # Seconds on an RTX 5090 - the trigger is the preroll, not the pixel count,
    # so this stays at 640x360. The ceiling is for a slower card, not for a
    # hang; a hang is what the timeout is meant to catch.
    set_tests_properties(NeuralRangeRenderSmoke PROPERTIES TIMEOUT 600)
    # A/V sync had no coverage at all: the audio clock every frame's due time
    # is computed from carried one assertion, that a number handed to Seek was
    # stored. This needs a real render endpoint, so it is labelled `audio` and
    # skips (125) where there is none.
    add_executable(AudioClockSmoke tests/AudioClockSmoke.cpp src/AudioPlayer.cpp src/WasapiRenderer.cpp)
    target_include_directories(AudioClockSmoke PRIVATE src)
    target_compile_definitions(AudioClockSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AudioClockSmoke PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_libraries(AudioClockSmoke PRIVATE winmm shell32 ole32)
    set_target_properties(AudioClockSmoke PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/audio-clock/$<CONFIG>")
    add_test(NAME AudioClockSmoke COMMAND AudioClockSmoke
        "${FFMPEG_STAGED_DIR}" "${CMAKE_BINARY_DIR}/audio-clock")
    set_tests_properties(AudioClockSmoke PROPERTIES LABELS audio SKIP_RETURN_CODE 125 TIMEOUT 300)

    add_executable(NeuralWorkerTests tests/NeuralWorkerTests.cpp src/NeuralWorker.cpp)
    target_include_directories(NeuralWorkerTests PRIVATE src)
    target_compile_definitions(NeuralWorkerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(NeuralWorkerTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_libraries(NeuralWorkerTests PRIVATE shell32)
    add_test(NAME NeuralWorkerTests COMMAND NeuralWorkerTests)
    add_executable(UpscalingTests tests/UpscalingTests.cpp)
    target_include_directories(UpscalingTests PRIVATE src)
    target_compile_definitions(UpscalingTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(UpscalingTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    # The resolve pass's HLSL is compiled headlessly by this suite, which needs no device
    # and no flow engine - only the compiler.
    target_link_libraries(UpscalingTests PRIVATE d3dcompiler)
    add_test(NAME UpscalingTests COMMAND UpscalingTests)
    add_executable(UpdateCheckTests tests/UpdateCheckTests.cpp src/UpdateCheck.cpp)
    target_include_directories(UpdateCheckTests PRIVATE src)
    target_compile_definitions(UpdateCheckTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}")
    target_compile_options(UpdateCheckTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_libraries(UpdateCheckTests PRIVATE winhttp)
    add_test(NAME UpdateCheckTests COMMAND UpdateCheckTests)
    add_executable(PolicyTests
        tests/PolicyTests.cpp
        tests/TestSupport.h
        src/AppMenu.h
        src/AppMenu.cpp
        src/UiLayout.cpp
        src/UiLayout.h
        src/UiResources.cpp
        src/UiResources.h
        src/GpuPreference.h
        src/RuntimePolicy.cpp
        src/RuntimePolicy.h
        # The frame grid the coverage predicates are measured against. Both live
        # here so a test can ask whether one step of the grid is work, which is
        # the question two sibling cases in this file used to answer differently.
        src/RangeSelection.cpp
        src/RangeSelection.h
        src/ReShadeConfig.cpp
        src/ReShadeConfig.h
        src/YouTubeResolver.cpp
        src/YouTubeResolver.h
        src/ExampleVideos.h
        src/VideoDecoder.cpp
        src/VideoDecoder.h
        src/MediaSource.cpp
        src/CompletionRegistry.h
        src/AudioPlayer.cpp src/WasapiRenderer.cpp
        src/AudioPlayer.h
        src/NetworkMediaTransaction.h
        src/D3D12FenceWait.h
        src/ParallelFor.h
        src/D3D12Renderer.cpp
        src/D3D12Renderer.h
        src/OpticalFlowNvof.cpp
        src/OpticalFlowNvof.h
        src/FlowGate.h
        src/NvofResolveShader.h
        src/PixelLayout.h
        src/DLSSBackend.cpp
        src/DLSSBackend.h
    )
    target_include_directories(PolicyTests PRIVATE src "${DLSS_SDK}/include" "${GENERATED_INCLUDE_DIR}")
    dlss_video_player_use_nvof(PolicyTests)
    target_compile_definitions(PolicyTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX YOUTUBE_RESOLVER_TESTING
    )
    target_compile_options(PolicyTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_directories(PolicyTests PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(PolicyTests PRIVATE d3d12 dxgi dxguid d3dcompiler shlwapi shell32 winhttp mfplat mfreadwrite mfuuid ole32 winmm ${DLSS_NGX_LIBRARY})
    add_dependencies(PolicyTests DLSSVideoPlayer)
    add_test(NAME PolicyTests COMMAND PolicyTests)

    add_executable(ReleaseApiCompileTests tests/ReleaseApiCompileTests.cpp)
    target_include_directories(ReleaseApiCompileTests PRIVATE src)
    target_compile_definitions(ReleaseApiCompileTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(ReleaseApiCompileTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    add_test(NAME ReleaseApiCompileTests COMMAND ReleaseApiCompileTests)

    add_executable(NeuralPrerenderTests
        tests/NeuralPrerenderTests.cpp
        tests/TestSupport.h
        src/NeuralCache.cpp
        src/NeuralCache.h
        src/NeuralPreflight.cpp
        src/NeuralPreflight.h
        src/MediaPipeline.cpp
        src/MediaPipeline.h
        src/OfflineNeuralRenderer.cpp
        src/NeuralRenderTypes.h
        src/OfflineNeuralRenderer.h
        src/SynchronizedPlayback.cpp
        src/SynchronizedPlayback.h
        src/VideoDecoder.cpp
        src/VideoDecoder.h
        src/MediaSource.cpp
        src/MediaSource.h
        src/D3D12Renderer.cpp
        src/DLSSBackend.cpp
        src/TemporalGuides.cpp
        src/OpticalFlowNvof.cpp
        # D3D12Renderer.cpp asks RuntimePolicy which adapter to create the
        # device on, the same reason PolicyTests and UpscalingGpuSmoke pair
        # these two.
        src/RuntimePolicy.cpp
    )
    target_include_directories(NeuralPrerenderTests PRIVATE src "${DLSS_SDK}/include" "${GENERATED_INCLUDE_DIR}")
    target_compile_definitions(NeuralPrerenderTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}"
    )
    target_compile_options(NeuralPrerenderTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_directories(NeuralPrerenderTests PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(NeuralPrerenderTests PRIVATE ${player_libraries})
    dlss_video_player_use_nvof(NeuralPrerenderTests)
    # The job's stage logging has always run here, and Log opens
    # <module directory>/DLSSVideoPlayer.log with ios::trunc, so running beside
    # the player erased its log on every ctest run. The cache-root probe also
    # stages a directory beside the executable. Give both their own place.
    set_target_properties(NeuralPrerenderTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/neural-prerender/$<CONFIG>")
    add_test(NAME NeuralPrerenderTests COMMAND NeuralPrerenderTests)

    # Exercise the real player completion handlers and text submitted to Win32.
    get_target_property(player_ui_sources DLSSVideoPlayer SOURCES)
    list(REMOVE_ITEM player_ui_sources src/main.cpp)
    add_executable(PlayerUiRegressionTests tests/PlayerUiRegressionTests.cpp ${player_ui_sources})
    target_include_directories(PlayerUiRegressionTests PRIVATE src "${DLSS_SDK}/include" "${GENERATED_INCLUDE_DIR}")
    dlss_video_player_use_nvof(PlayerUiRegressionTests)
    target_compile_definitions(PlayerUiRegressionTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX PLAYER_APP_TESTING
        DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}")
    get_target_property(player_ui_options DLSSVideoPlayer COMPILE_OPTIONS)
    target_compile_options(PlayerUiRegressionTests PRIVATE ${player_ui_options})
    get_target_property(player_ui_libraries DLSSVideoPlayer LINK_LIBRARIES)
    target_link_directories(PlayerUiRegressionTests PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(PlayerUiRegressionTests PRIVATE ${player_ui_libraries})
    # The player persists settings beside its executable; isolate test settings.
    set_target_properties(PlayerUiRegressionTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ui-regressions/$<CONFIG>")
    # NetworkPreparedRendererSmoke drives the real PrepareYouTubeMedia, and the
    # VideoDecoder it builds finds its helpers beside its own module - which for
    # this target is the test's own output directory, not the player's. Staged
    # the same way the player stages them; without it the decoder reports
    # "Bundled/system FFmpeg tools not found" and the smoke cannot open anything.
    if(EXISTS "${FFMPEG_STAGED_DIR}/ffmpeg.exe" AND EXISTS "${FFMPEG_STAGED_DIR}/ffprobe.exe")
        add_custom_command(TARGET PlayerUiRegressionTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_STAGED_DIR}/ffmpeg.exe"
                "$<TARGET_FILE_DIR:PlayerUiRegressionTests>/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_STAGED_DIR}/ffprobe.exe"
                "$<TARGET_FILE_DIR:PlayerUiRegressionTests>/ffprobe.exe"
        )
    endif()
    add_test(NAME PlayerUiRegressionTests COMMAND PlayerUiRegressionTests)
    # The same binary, its `--gpu` case set. Nothing in this suite opened a
    # network source, and the prepared-renderer commit path is reached by
    # nothing else - four defects lived there at once while 23 tests stayed
    # green. A second target would compile main.cpp a second time, which is the
    # amplification 2.12 is about, so this registers the existing one again.
    add_test(NAME NetworkPreparedRendererSmoke COMMAND PlayerUiRegressionTests
        --gpu "${FFMPEG_STAGED_DIR}" "${CMAKE_BINARY_DIR}/network-prepared")
    set_tests_properties(NetworkPreparedRendererSmoke PROPERTIES
        LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 300)

    # Every test ends: a hung helper child or a deadlocked wait fails the run
    # instead of stalling it. Each budget is several times its measured Release
    # run time on the reference machine; NeuralWorkerTests sleeps a child for
    # 20 s and the hardware smokes render real frames.
    set_tests_properties(RecentMediaTests RenderSettingsTests RuntimeLockTests RangeSelectionTests
        UpscalingTests ReleaseApiCompileTests UpdateCheckTests PROPERTIES TIMEOUT 60)
    # DlssgProbeSmoke creates one feature and no frames, so it belongs with the
    # 300 s budgets rather than with the two smokes that render.
    set_tests_properties(FrameIdentityTests CachedExportTests PlayerUiRegressionTests DlssgProbeSmoke
        PROPERTIES TIMEOUT 300)
    set_tests_properties(NeuralWorkerTests PolicyTests NeuralPrerenderTests PROPERTIES TIMEOUT 600)
    # FrameGenerationSmoke decodes, generates and encodes a whole clip, so it
    # belongs with the two smokes that render rather than with the 300 s probes.
    set_tests_properties(UpscalingGpuSmoke MediaGpuSmoke FrameGenerationSmoke PROPERTIES TIMEOUT 900)

    # A test seam (PLAYER_APP_TESTING, YOUTUBE_RESOLVER_TESTING, ...) opens a
    # private route that only a test may take; none may be compiled into a
    # shipped binary. The YouTube seam is proven below by compiling against the
    # release header; this catches every seam by its name on the two targets
    # that ship.
    get_directory_property(shipped_directory_definitions COMPILE_DEFINITIONS)
    foreach(shipped_target DLSSVideoPlayer NeuralWorker)
        get_target_property(shipped_definitions ${shipped_target} COMPILE_DEFINITIONS)
        get_target_property(shipped_options ${shipped_target} COMPILE_OPTIONS)
        foreach(entry IN LISTS shipped_directory_definitions shipped_definitions shipped_options)
            if(entry MATCHES "_TESTING(=|$)")
                message(FATAL_ERROR "${shipped_target} compiles with the test seam ${entry}.")
            endif()
        endforeach()
    endforeach()

    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_SOURCE_DIR}/src")
    set(CMAKE_REQUIRED_DEFINITIONS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    unset(YOUTUBE_RELEASE_EXPOSES_FAILURE_STAGE CACHE)
    check_cxx_source_compiles([[#include "YouTubeResolver.h"
int main() { auto value = YouTubeResolver::FailureStage::None; return static_cast<int>(value); }
]] YOUTUBE_RELEASE_EXPOSES_FAILURE_STAGE)
    if(YOUTUBE_RELEASE_EXPOSES_FAILURE_STAGE)
        message(FATAL_ERROR "Release YouTubeResolver exposes its test-only failure stages.")
    endif()
    unset(YOUTUBE_RELEASE_EXPOSES_FORMER_FRIEND_ROUTE CACHE)
    check_cxx_source_compiles([[#include "YouTubeResolver.h"
#include <utility>
struct YouTubeResolverTestAccess {
    static YouTubeResolver Create() {
        YouTubeResolver::Settings settings;
        return YouTubeResolver(std::move(settings));
    }
};
int main() { auto resolver = YouTubeResolverTestAccess::Create(); (void)resolver; return 0; }
]] YOUTUBE_RELEASE_EXPOSES_FORMER_FRIEND_ROUTE)
    if(YOUTUBE_RELEASE_EXPOSES_FORMER_FRIEND_ROUTE)
        message(FATAL_ERROR "Release YouTubeResolver exposes its former friend injection route.")
    endif()
    set(CMAKE_REQUIRED_DEFINITIONS
        -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DYOUTUBE_RESOLVER_TESTING)
    unset(YOUTUBE_TEST_BUILD_SUPPORTS_FRIEND_ROUTE CACHE)
    check_cxx_source_compiles([[#include "YouTubeResolver.h"
#include <utility>
struct YouTubeResolverTestAccess {
    static YouTubeResolver Create() {
        YouTubeResolver::Settings settings;
        return YouTubeResolver(std::move(settings));
    }
};
int main() { auto resolver = YouTubeResolverTestAccess::Create(); (void)resolver; return 0; }
]] YOUTUBE_TEST_BUILD_SUPPORTS_FRIEND_ROUTE)
    if(NOT YOUTUBE_TEST_BUILD_SUPPORTS_FRIEND_ROUTE)
        message(FATAL_ERROR "PolicyTests compile definition does not enable its friend injection route.")
    endif()
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_DEFINITIONS)
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
