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
# projects side by side; this is the other axis. Since the sources moved into
# the object libraries below (57 compiles for the whole tree, down from 181),
# the long pole is the largest of those, player_app, with 13 TUs.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/MP>)

# Every target is at zero warnings under /W4, so a new one is an error rather
# than a line nobody reads in a green build log.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX>)

# The quality job in build.yml builds two more configurations of this tree.
# Neither is for shipping: ASan instruments every object, and /analyze only
# adds diagnostics.
option(DLSS_VIDEO_PLAYER_ASAN "Build with AddressSanitizer, for the device-free test suites" OFF)
option(DLSS_VIDEO_PLAYER_ANALYZE "Run the MSVC code analyzer (/analyze) on the two shipped targets" OFF)
if(DLSS_VIDEO_PLAYER_ASAN)
    # /Zi so ASan reports carry file and line. The shipped targets' /guard:cf
    # and /INCREMENTAL-free Release link are compatible with it.
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/fsanitize=address> $<$<COMPILE_LANGUAGE:CXX>:/Zi>)
    add_link_options(/DEBUG)
    # The instrumented executables load clang_rt.asan_dynamic-x86_64.dll, which
    # lives beside cl.exe rather than on PATH; every test gets that directory.
    get_filename_component(DLSS_VIDEO_PLAYER_ASAN_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
endif()

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

# The NVENC API header lives in external/nvenc under the same file-scoped MIT notice,
# pinned from FFmpeg's nv-codec-headers. It lets the neural capture be encoded straight
# from the D3D12 texture (NvencDirect.cpp); nvEncodeAPI64.dll comes from the driver at
# run time and no import library is linked. Absent, every render takes the ffmpeg
# encoder child, which is also the fallback when the driver refuses the direct path.
set(NVENC_SDK "${CMAKE_SOURCE_DIR}/external/nvenc" CACHE PATH "Path to the NVENC API header (nvEncodeAPI.h)")
if(EXISTS "${NVENC_SDK}/nvEncodeAPI.h")
    set(NVENC_FOUND TRUE)
    message(STATUS "NVENC API header found at ${NVENC_SDK}; direct D3D12 encoding enabled.")
else()
    set(NVENC_FOUND FALSE)
    message(WARNING "The NVENC API header is missing from ${NVENC_SDK}. It ships with this repository, so this build will encode every neural render through the ffmpeg child - see docs/BUILDING.md.")
endif()

# Control Flow Guard and CET shadow stacks for the two shipped binaries. The
# compiler and linker already default to /GS, /DYNAMICBASE, /NXCOMPAT and
# /HIGHENTROPYVA; they are spelled out so a toolset that stops defaulting them
# shows up as a diff here rather than in dumpbin output of a shipped package.
# The compile half, /guard:cf /GS, is in dlss_video_player_shipped_code below.
function(dlss_video_player_harden target)
    target_link_options(${target} PRIVATE /GUARD:CF /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
endfunction()

# Symbols and reproducibility for the two shipped binaries. CrashDump.h writes
# minidumps from both processes, and without a PDB that matches the build a
# dump cannot be symbolised, so their code compiles with /Zi and they link
# with /DEBUG. /DEBUG turns the linker's default /OPT:REF and /OPT:ICF off,
# which would grow and change the shipped code, so they are stated again.
# /PDBALTPATH keeps only the PDB's file name in the executable instead of the
# builder's absolute path - the complete package is linked on a maintainer's
# machine, under their user folder. /Brepro (compiled in as well) replaces the
# link timestamp and the PDB signature with hashes of the content, so the same
# source, toolset and build path give the same bytes and a rebuild can be
# checked against a published package. The PDBs stay beside the binaries in
# the build tree; the packager copies an explicit list that does not name
# them, and CI uploads them separately.
function(dlss_video_player_symbols target)
    target_link_options(${target} PRIVATE /DEBUG /PDBALTPATH:$<TARGET_PDB_FILE_NAME:${target}>
        "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF;/Brepro>")
endfunction()

# The code analyzer runs on the code that ships (dlss_video_player_shipped_code
# below). Findings in the SDK and Windows headers are not this project's to
# fix, so external headers are excluded.
# Under /WX every finding fails the build, so each one below was read and is
# suppressed for a stated reason (first run, 2026-09-23, 11 findings):
#   C6262 x6   stack frames of 21-66 KiB: 32K-wchar long-path buffers and the
#              one-time initializer-list of the English string table, on
#              threads with the default 1 MiB stack. The 16 KiB threshold is
#              raised to 128 KiB rather than the warning switched off, so a
#              frame twice the largest of these still fails.
#   C28251     wWinMain's definition does not repeat the SDK's SAL
#              annotations; cosmetic.
#   C6336      main.cpp `size()%2?a:b` in MedianMsPerFrame: the precedence is
#              the intended one.
#   C6387 x2   main.cpp DrawButton and Paint select an HFONT/HBITMAP that the
#              analyzer cannot see is non-null: showIcon requires m_iconFont,
#              and failedStage is set whenever the bitmap is null.
#   C28020     NeuralWorker.cpp StartHelper fills a 4-slot handle array from at
#              most four sources; the analyzer cannot bound the counter.
# The last three are scoped to their files and belong in the source as
# `#pragma warning(suppress: ...)` next to each line when those files are next
# edited.
if(DLSS_VIDEO_PLAYER_ANALYZE)
    set_property(SOURCE src/main.cpp APPEND PROPERTY COMPILE_OPTIONS /wd6336 /wd6387)
    set_property(SOURCE src/NeuralWorker.cpp APPEND PROPERTY COMPILE_OPTIONS /wd28020)
endif()

# The packaging lock is embedded so the player and helper verify the exact
# runtime files at run time, not only at packaging time.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/packaging/runtime-lock.json")
file(READ "${CMAKE_SOURCE_DIR}/packaging/runtime-lock.json" RUNTIME_LOCK_JSON)
configure_file(src/RuntimeLockData.h.in "${CMAKE_BINARY_DIR}/generated/RuntimeLockData.h" @ONLY)
configure_file(src/VersionData.h.in "${CMAKE_BINARY_DIR}/generated/VersionData.h" @ONLY)
set(GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")

# The NVIDIA RTX Video SDK (P2.8), optional and never committed: it is behind an
# NVIDIA developer login and its licence allows it to ship only as object code in
# an application (docs/BUILDING.md, THIRD_PARTY.md). Empty, as on CI, compiles the
# RTX VSR engine out and everything builds and tests as it did before. Set, the
# engine is compiled against the SDK's VSR feature header and its release
# nvngx_vsr.dll is staged beside the player. It links no library of its own: the
# DLSS SDK's NGX core already linked creates feature 16 (VsrEngine.h).
set(RTX_VIDEO_SDK "" CACHE PATH "Path to the NVIDIA RTX Video SDK (optional; builds the RTX VSR comparison view)")
set(RTX_VSR_FOUND FALSE)
if(RTX_VIDEO_SDK)
    set(RTX_VSR_RUNTIME "${RTX_VIDEO_SDK}/bin/Windows/x64/rel/nvngx_vsr.dll")
    if(NOT EXISTS "${RTX_VIDEO_SDK}/include/nvsdk_ngx_helpers_vsr.h" OR NOT EXISTS "${RTX_VSR_RUNTIME}")
        message(FATAL_ERROR "RTX_VIDEO_SDK is ${RTX_VIDEO_SDK}, which holds no include/nvsdk_ngx_helpers_vsr.h or bin/Windows/x64/rel/nvngx_vsr.dll. Point it at the extracted RTX Video SDK (see docs/BUILDING.md), or leave it empty to build without RTX VSR.")
    endif()
    set(RTX_VSR_FOUND TRUE)
    # The DLSS SDK's helpers first, then the VSR SDK's by absolute path: the VSR
    # SDK ships its own copies of the NGX core headers under the same include
    # guards, so the ones the linked core was built from stay in force. Angle
    # brackets keep both external (/external:anglebrackets), like every NGX header.
    file(CONFIGURE OUTPUT "${GENERATED_INCLUDE_DIR}/RtxVideoSdkHeaders.h"
        CONTENT "#pragma once\n#include <nvsdk_ngx_helpers.h>\n#include <${RTX_VIDEO_SDK}/include/nvsdk_ngx_helpers_vsr.h>\n")
    message(STATUS "NVIDIA RTX Video SDK found at ${RTX_VIDEO_SDK}; the RTX VSR comparison view is built.")
endif()

# Link-time code generation for Release, where the toolset supports it. Not in
# the sanitizer or analyzer configurations, which are not measured and would
# only link slower. Must be decided before the targets below exist.
option(DLSS_VIDEO_PLAYER_IPO "Link-time code generation (/GL, /LTCG) for Release" ON)
if(DLSS_VIDEO_PLAYER_IPO AND NOT DLSS_VIDEO_PLAYER_ASAN AND NOT DLSS_VIDEO_PLAYER_ANALYZE)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT DLSS_VIDEO_PLAYER_IPO OUTPUT DLSS_VIDEO_PLAYER_IPO_OUTPUT LANGUAGES CXX)
    if(DLSS_VIDEO_PLAYER_IPO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        # Full, not incremental, link-time code generation. With whole-program
        # optimization on and nothing stated, MSBuild links with
        # /LTCG:INCREMENTAL and keeps a .iobj/.ipdb database between builds.
        # On 2026-09-23 a stale database made an incremental relink of
        # PolicyTests produce a binary that failed a decoder test on every run
        # and passed on every run once the database was deleted, and later
        # crashed the linker outright (C1001, LNK1000). A shipped binary built
        # the same way could be just as wrong, so no build here reuses one.
        add_link_options($<$<CONFIG:Release>:/LTCG>)
    else()
        message(STATUS "Interprocedural optimization is not available: ${DLSS_VIDEO_PLAYER_IPO_OUTPUT}")
    endif()
endif()

# One set of options for every target in the tree, tests included. Each target
# used to spell its own, and they had drifted: six test targets compiled
# without /Zc:__cplusplus and UpscalingGpuSmoke without the generated include
# directory, so the tests were not running the object code that ships.
add_library(dlss_video_player_common INTERFACE)
target_compile_definitions(dlss_video_player_common INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}")
target_compile_options(dlss_video_player_common INTERFACE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(dlss_video_player_common INTERFACE
    "${CMAKE_SOURCE_DIR}/src" "${GENERATED_INCLUDE_DIR}" "${DLSS_SDK}/include")
target_link_directories(dlss_video_player_common INTERFACE "${DLSS_LIB_DIR}")
target_link_libraries(dlss_video_player_common INTERFACE
    d3d12 d3d11 dxgi dxguid d3dcompiler
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi winhttp bcrypt dbghelp wintrust
    ${DLSS_NGX_LIBRARY})

# How the code that ships is compiled, wherever it is compiled: Control Flow
# Guard (/guard:cf only protects functions compiled with it, so it belongs on
# every object the two binaries link, not only on their own sources), symbols
# and reproducible output (see dlss_video_player_symbols) and, in the analyze
# configuration, the code analyzer. The object libraries below compile with
# it, so the tests run these very objects.
add_library(dlss_video_player_shipped_code INTERFACE)
target_compile_options(dlss_video_player_shipped_code INTERFACE /guard:cf /GS /Zi /Brepro)
if(DLSS_VIDEO_PLAYER_ANALYZE)
    target_compile_options(dlss_video_player_shipped_code INTERFACE
        /analyze /analyze:external- /external:anglebrackets /external:W0
        "SHELL:/analyze:stacksize 131072" /wd28251)
endif()

# One set of objects. Every source used to be compiled once per target that
# listed it - 181 compiles of about 55 files, VideoDecoder and MediaSource a
# dozen times each - so a test linked its own copy, built with its own flags.
# Each library below is compiled once and linked whole, and the split follows
# what the two binaries link: NeuralWorker is media + render + neural, exactly
# the objects it always had, and the player adds app. A test links all four,
# because an object library brings every object and they reference each
# other. Sources that differ per binary stay with it: main.cpp,
# NeuralWorkerMain.cpp, NeuralPreflightProbe.cpp, the exported driver hints in
# GpuPreference.cpp (which a test must not export) and YouTubeResolver.cpp,
# whose PolicyTests build carries a test seam that changes its layout.
add_library(player_media OBJECT
    src/VideoDecoder.cpp src/VideoDecoder.h
    src/HdrToneMapGpu.cpp src/HdrToneMapGpu.h src/HdrToneMap.h
    src/MediaSource.cpp src/MediaSource.h
    src/MediaPipeline.cpp src/MediaPipeline.h)
add_library(player_render OBJECT
    src/D3D12Renderer.cpp src/D3D12Renderer.h
    src/DLSSBackend.cpp src/DLSSBackend.h
    src/VsrEngine.cpp src/VsrEngine.h src/VsrPolicy.h
    src/TemporalGuides.cpp src/TemporalGuides.h
    src/OpticalFlowNvof.cpp src/OpticalFlowNvof.h
    src/NvencDirect.cpp src/NvencDirect.h src/NvencDirectPolicy.h src/NvencDirectMux.h
    src/RuntimePolicy.cpp src/RuntimePolicy.h)
add_library(player_neural OBJECT
    src/NeuralWorker.cpp src/NeuralWorker.h
    src/NeuralPreflight.cpp src/NeuralPreflight.h
    src/NeuralCache.cpp src/NeuralCache.h
    src/RuntimeLock.cpp src/RuntimeLock.h
    src/OfflineNeuralRenderer.cpp src/OfflineNeuralRenderer.h
    src/ReShadeConfig.cpp src/ReShadeConfig.h)
# What only the player links.
add_library(player_app OBJECT
    src/AppMenu.cpp src/AppMenu.h
    src/UiLayout.cpp src/UiLayout.h
    src/UiResources.cpp src/UiResources.h
    src/DLSSGBackend.cpp src/DLSSGBackend.h
    src/FrameGenerationPass.cpp src/FrameGenerationPass.h
    src/AudioPlayer.cpp src/AudioPlayer.h src/WasapiRenderer.cpp
    src/SubtitleOverlay.cpp src/SubtitleOverlay.h src/SubtitlePolicy.h
    src/RecentMedia.cpp src/RecentMedia.h
    src/SynchronizedPlayback.cpp src/SynchronizedPlayback.h
    src/NeuralReceipt.cpp src/NeuralReceipt.h
    src/NeuralSettings.cpp src/NeuralSettings.h
    src/RangeSelection.cpp src/RangeSelection.h
    src/UpdateCheck.cpp src/UpdateCheck.h
    src/TrailerThumbnail.cpp src/TrailerThumbnail.h src/TrailerThumbnailPolicy.h)
set(PLAYER_OBJECT_LIBRARIES player_media player_render player_neural player_app)
foreach(object_library IN LISTS PLAYER_OBJECT_LIBRARIES)
    target_link_libraries(${object_library} PUBLIC dlss_video_player_common PRIVATE dlss_video_player_shipped_code)
endforeach()
# Every target that compiles D3D12Renderer.cpp also compiles the flow backend.
# PUBLIC so a test that includes the renderer's headers sees the same
# definition the objects were built with.
if(NVOF_FOUND)
    target_include_directories(player_render PUBLIC "${NVOF_SDK}")
    target_compile_definitions(player_render PUBLIC DLSS_VIDEO_PLAYER_HAS_NVOF)
endif()
# The same for the direct NVENC path; its policy header is tested without a GPU,
# so the tests see the definition the objects were built with.
if(NVENC_FOUND)
    target_include_directories(player_render PUBLIC "${NVENC_SDK}")
    target_compile_definitions(player_render PUBLIC DLSS_VIDEO_PLAYER_HAS_NVENC)
endif()
# The RTX VSR engine behind its one switch, PUBLIC for the same reason.
if(RTX_VSR_FOUND)
    target_compile_definitions(player_render PUBLIC DLSS_VIDEO_PLAYER_HAS_RTX_VSR)
endif()

add_executable(DLSSVideoPlayer WIN32
    src/main.cpp
    src/resources.h
    src/resources.rc
    # Common Controls 6 and per-monitor v2 DPI, merged into the linker's manifest.
    src/DLSSVideoPlayer.manifest
    src/VersionData.h.in
    src/RuntimeLockData.h.in
    src/FrameRatePolicy.h
    src/FlowGate.h
    src/NvofResolveShader.h
    src/HardErrorSuppression.h
    src/D3D12FenceWait.h
    src/PixelLayout.h
    src/ParallelFor.h
    src/Log.h
    src/Localization.h
    # Exported driver hints; the comment in GpuPreference.cpp explains why they
    # are a translation unit of their own and why both binaries compile it.
    src/GpuPreference.cpp
    src/GpuPreference.h
    src/YouTubeResolver.cpp
    src/YouTubeResolver.h
    src/ExampleVideos.h
    src/CompletionRegistry.h
    src/NetworkMediaTransaction.h
    src/NeuralRenderTypes.h
    src/ResidentHelperPolicy.h
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
)
target_link_libraries(DLSSVideoPlayer PRIVATE ${PLAYER_OBJECT_LIBRARIES} dlss_video_player_shipped_code)
dlss_video_player_harden(DLSSVideoPlayer)
dlss_video_player_symbols(DLSSVideoPlayer)

# Only the offline helper loads the experimental proxy/add-on. The player root
# stays hook-free for its independent runtime Super Resolution feature.
add_executable(NeuralWorker src/NeuralWorkerMain.cpp src/NeuralPreflightProbe.cpp src/GpuPreference.cpp)
target_link_libraries(NeuralWorker PRIVATE player_media player_render player_neural dlss_video_player_shipped_code)
dlss_video_player_harden(NeuralWorker)
dlss_video_player_symbols(NeuralWorker)
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
# The release feature DLL only, in every configuration: the SDK's dev build draws a
# watermark over the picture (RTX Video SDK Programming Guide 1.1.0, chapter 4).
if(RTX_VSR_FOUND)
    add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RTX_VSR_RUNTIME}"
            "$<TARGET_FILE_DIR:DLSSVideoPlayer>/nvngx_vsr.dll"
    )
endif()

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
    # Every test links the player's object libraries - the objects that ship -
    # and through them the common option set, rather than compiling its own
    # copy of the sources it exercises.
    function(dlss_video_player_test target)
        add_executable(${target} ${ARGN})
        target_link_libraries(${target} PRIVATE ${PLAYER_OBJECT_LIBRARIES})
    endfunction()

    dlss_video_player_test(RecentMediaTests tests/RecentMediaTests.cpp)
    dlss_video_player_test(RenderSettingsTests tests/RenderSettingsTests.cpp)
    # CachedExportTests drives SynchronizedPlayback through its owning constructor,
    # which is the only place the decoder-backed frame sources are built, and feeds a
    # real join's probe into the publish gate in RuntimePolicy.
    dlss_video_player_test(CachedExportTests tests/CachedExportTests.cpp)
    dlss_video_player_test(RuntimeLockTests tests/RuntimeLockTests.cpp)
    dlss_video_player_test(RangeSelectionTests tests/RangeSelectionTests.cpp)
    dlss_video_player_test(FrameIdentityTests tests/FrameIdentityTests.cpp)
    target_link_libraries(RuntimeLockTests PRIVATE version)
    add_test(NAME RecentMediaTests COMMAND RecentMediaTests)
    add_test(NAME RenderSettingsTests COMMAND RenderSettingsTests)
    add_test(NAME CachedExportTests COMMAND CachedExportTests "${FFMPEG_STAGED_DIR}")
    # FFmpeg is optional at configure time (FFMPEG_STAGED_DIR above), so a tree
    # without it reports this real-media suite as skipped rather than failed.
    set_tests_properties(CachedExportTests PROPERTIES SKIP_RETURN_CODE 125)
    add_test(NAME RuntimeLockTests COMMAND RuntimeLockTests)
    add_test(NAME RangeSelectionTests COMMAND RangeSelectionTests)
    add_test(NAME FrameIdentityTests COMMAND FrameIdentityTests)
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
        PROPERTIES LABELS gpu SKIP_RETURN_CODE 125)
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

    dlss_video_player_test(NeuralWorkerTests tests/NeuralWorkerTests.cpp)
    add_test(NAME NeuralWorkerTests COMMAND NeuralWorkerTests)
    # The resolve pass's HLSL is compiled headlessly by this suite, which needs no device
    # and no flow engine - only the compiler. It links no player objects.
    add_executable(UpscalingTests tests/UpscalingTests.cpp)
    target_link_libraries(UpscalingTests PRIVATE dlss_video_player_common)
    add_test(NAME UpscalingTests COMMAND UpscalingTests)
    dlss_video_player_test(UpdateCheckTests tests/UpdateCheckTests.cpp)
    add_test(NAME UpdateCheckTests COMMAND UpdateCheckTests)
    # YouTubeResolver.cpp is compiled here rather than linked from the player's
    # objects: YOUTUBE_RESOLVER_TESTING opens its test seam, which changes the
    # class's layout, so this build of it must not meet the shipped one.
    dlss_video_player_test(PolicyTests
        tests/PolicyTests.cpp
        tests/TestSupport.h
        src/YouTubeResolver.cpp
        src/YouTubeResolver.h
    )
    target_compile_definitions(PolicyTests PRIVATE YOUTUBE_RESOLVER_TESTING)
    add_dependencies(PolicyTests DLSSVideoPlayer)
    add_test(NAME PolicyTests COMMAND PolicyTests)

    add_executable(ReleaseApiCompileTests tests/ReleaseApiCompileTests.cpp)
    target_link_libraries(ReleaseApiCompileTests PRIVATE dlss_video_player_common)
    add_test(NAME ReleaseApiCompileTests COMMAND ReleaseApiCompileTests)

    dlss_video_player_test(NeuralPrerenderTests tests/NeuralPrerenderTests.cpp tests/TestSupport.h)
    # The job's stage logging has always run here, and Log opens
    # <module directory>/DLSSVideoPlayer.log with ios::trunc, so running beside
    # the player erased its log on every ctest run. The cache-root probe also
    # stages a directory beside the executable. Give both their own place.
    set_target_properties(NeuralPrerenderTests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/neural-prerender/$<CONFIG>")
    add_test(NAME NeuralPrerenderTests COMMAND NeuralPrerenderTests)

    # Exercise the real player completion handlers and text submitted to Win32.
    # The test includes main.cpp itself, so it takes the player's other own
    # sources (not main.cpp) beside the object libraries.
    get_target_property(player_ui_sources DLSSVideoPlayer SOURCES)
    # Nor the manifest: its per-monitor DPI awareness would run the fixture at the
    # desktop's scale, and the layouts these tests pin are measured at 96 dpi.
    list(REMOVE_ITEM player_ui_sources src/main.cpp src/DLSSVideoPlayer.manifest)
    dlss_video_player_test(PlayerUiRegressionTests tests/PlayerUiRegressionTests.cpp ${player_ui_sources})
    target_compile_definitions(PlayerUiRegressionTests PRIVATE PLAYER_APP_TESTING)
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
    set_tests_properties(NeuralPreflightSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 300)
    # Seven renders of a 3.5 s clip, two of them at 1440p. Measured at 96 s.
    set_tests_properties(ExportMatrixSmoke PROPERTIES LABELS gpu SKIP_RETURN_CODE 125 TIMEOUT 900)
    # FrameGenerationSmoke decodes, generates and encodes a whole clip, so it
    # belongs with the two smokes that render rather than with the 300 s probes.
    set_tests_properties(UpscalingGpuSmoke UpscalingSrQualitySmoke MediaGpuSmoke FrameGenerationSmoke PROPERTIES TIMEOUT 900)

    # A test seam (PLAYER_APP_TESTING, YOUTUBE_RESOLVER_TESTING, ...) opens a
    # private route that only a test may take; none may be compiled into a
    # shipped binary. The YouTube seam is proven below by compiling against the
    # release header; this catches every seam by its name on the two targets
    # that ship.
    # The two binaries now take most of their objects, and every definition,
    # from the object libraries and the interface targets, so those are checked
    # as well as the binaries themselves.
    get_directory_property(shipped_directory_definitions COMPILE_DEFINITIONS)
    foreach(shipped_target DLSSVideoPlayer NeuralWorker ${PLAYER_OBJECT_LIBRARIES}
            dlss_video_player_common dlss_video_player_shipped_code)
        get_target_property(shipped_type ${shipped_target} TYPE)
        if(shipped_type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(shipped_definitions ${shipped_target} INTERFACE_COMPILE_DEFINITIONS)
            get_target_property(shipped_options ${shipped_target} INTERFACE_COMPILE_OPTIONS)
        else()
            get_target_property(shipped_definitions ${shipped_target} COMPILE_DEFINITIONS)
            get_target_property(shipped_options ${shipped_target} COMPILE_OPTIONS)
        endif()
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

    if(DLSS_VIDEO_PLAYER_ASAN)
        get_property(asan_tests DIRECTORY PROPERTY TESTS)
        set_property(TEST ${asan_tests} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${DLSS_VIDEO_PLAYER_ASAN_RUNTIME_DIR}")
    endif()
endif()
