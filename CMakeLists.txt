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
# The C and C++ runtimes are linked statically (/MT, /MTd for Debug). With the
# DLL runtime both shipped executables imported MSVCP140, MSVCP140_ATOMIC_WAIT
# and VCRUNTIME140(_1), which no package carried and no document asked for: a
# clean machine could not start the player at all, before it had written a log,
# and a Visual C++ Redistributable older than 14.40 crashes in
# std::mutex::lock at the first log line (the constexpr mutex constructor of
# MSVC 17.10). CI runners always have the newest one, so no test could see it.
# Every module the executables talk to - NGX, NVENC, NVOF, ReShade, Streamline
# - is its own image behind a C API that passes no CRT object across, so a
# private runtime per image is safe. ShippedRuntimeImports below fails if
# either shipped executable imports a runtime DLL again.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
# main.cpp outgrew the default COFF section limit under /fsanitize=address
# (C1128 on CI's ASan build); /bigobj lifts it for every configuration.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/bigobj>)

# Every target is at zero warnings under /W4, so a new one is an error rather
# than a line nobody reads in a green build log.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX>)

# The quality job in build.yml builds two more configurations of this tree.
# Neither is for shipping: ASan instruments every object, and /analyze only
# adds diagnostics.
option(DLSS_VIDEO_PLAYER_ASAN "Build with AddressSanitizer, for the device-free test suites" OFF)
option(DLSS_VIDEO_PLAYER_ANALYZE "Run the MSVC code analyzer (/analyze) on the three shipped targets" OFF)
option(DLSS_VIDEO_PLAYER_HARDWARE_TESTS "Build and register the GPU and audio smokes (tests/HardwareTests.cmake)" ON)
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
# NGX ships one library per CRT: the _s pair is built against the static runtime
# that CMAKE_MSVC_RUNTIME_LIBRARY above selects, the _d pair against the DLL one,
# and each pair has a _dbg build against the debug CRT at _ITERATOR_DEBUG_LEVEL 2,
# which is what a Debug build here compiles with. Linking a library of the other
# CRT fails with LNK2038 (RuntimeLibrary or _ITERATOR_DEBUG_LEVEL mismatch) or
# pulls a second CRT in under LNK4098, so the choice is made once here and every
# target that names NGX links this variable.
set(DLSS_NGX_LIBRARY "$<IF:$<CONFIG:Debug>,nvsdk_ngx_s_dbg,nvsdk_ngx_s>")
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

# Control Flow Guard and CET shadow stacks for the three shipped binaries. The
# compiler and linker already default to /GS, /DYNAMICBASE, /NXCOMPAT and
# /HIGHENTROPYVA; they are spelled out so a toolset that stops defaulting them
# shows up as a diff here rather than in dumpbin output of a shipped package.
# The compile half, /guard:cf /GS, is in dlss_video_player_shipped_code below.
function(dlss_video_player_harden target)
    target_link_options(${target} PRIVATE /GUARD:CF /CETCOMPAT /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
endfunction()

# Symbols and reproducibility for the three shipped binaries. CrashDump.h
# writes minidumps from the player and the helper, and without a PDB that
# matches the build a dump cannot be symbolised, so their code compiles with
# /Zi and they link with /DEBUG. /DEBUG turns the linker's default /OPT:REF and /OPT:ICF off,
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
# tools/stage_rtx_video_sdk.ps1 puts the SDK in external/rtx-video-sdk; a
# checkout that has it there builds the view without being told. A plain
# variable rather than the cache, so removing the folder turns it off again.
if(NOT RTX_VIDEO_SDK AND EXISTS "${CMAKE_SOURCE_DIR}/external/rtx-video-sdk/include/nvsdk_ngx_helpers_vsr.h"
   AND EXISTS "${CMAKE_SOURCE_DIR}/external/rtx-video-sdk/bin/Windows/x64/rel/nvngx_vsr.dll")
    set(RTX_VIDEO_SDK "${CMAKE_SOURCE_DIR}/external/rtx-video-sdk")
endif()
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
    message(STATUS "NVIDIA RTX Video SDK found at ${RTX_VIDEO_SDK}; RTX VSR (the playback and export upscaler, and its comparison view) is built.")
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
    src/VsrUpscalePass.cpp src/VsrUpscalePass.h
    # Export with DLSS stages: the dialog and --render run these passes.
    src/StageExport.cpp src/StageExport.h
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
    src/BackgroundFileReaper.h
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
# Load-time imports resolve from System32 only (P1.29). None of dxgi, d3d12,
# d3d11, D3DCOMPILER_47, dbghelp, VERSION, WINHTTP or dwmapi is a KnownDLL, and
# SetDefaultDllDirectories in wWinMain runs after the loader has resolved them,
# so a dxgi.dll beside the exe - the old layout's ReShade proxy, or a planted
# one - loaded before the guard that refuses it could run. 0x800 is
# LOAD_LIBRARY_SEARCH_SYSTEM32, and every static import of the player lives
# there (PlayerDllSearchPinned checks the flag). Not on NeuralWorker, which
# needs the proxy dxgi.dll beside it, and not under ASan, whose runtime DLL is
# found beside the executable.
# One DLL is loaded from beside the player on purpose: the dlssg_sm86 Frame
# Generation add-on, by full path and only with its ini there (FrameGenAddon.h).
# LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR lets its own imports resolve beside it too,
# so while it is loaded the player is no longer System32-only.
if(NOT DLSS_VIDEO_PLAYER_ASAN)
    target_link_options(DLSSVideoPlayer PRIVATE /DEPENDENTLOADFLAG:0x800)
endif()

# Only the offline helper loads the experimental proxy/add-on. The player root
# stays hook-free for its independent runtime Super Resolution feature.
add_executable(NeuralWorker src/NeuralWorkerMain.cpp src/NeuralPreflightProbe.cpp src/GpuPreference.cpp)
target_link_libraries(NeuralWorker PRIVATE player_media player_render player_neural dlss_video_player_shipped_code)
dlss_video_player_harden(NeuralWorker)
dlss_video_player_symbols(NeuralWorker)
set_target_properties(NeuralWorker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/neural-runtime")

# dlss5-convert.exe: the console front end for the player's --render and
# --probe (ConvertCommandLine.h). A console program, so a shell waits for it,
# shows its progress and hands it Ctrl+C; it runs DLSSVideoPlayer.exe from its
# own folder for every conversion, so there is still one pipeline.
add_executable(dlss5-convert src/ConvertMain.cpp src/ConvertCommandLine.h src/RenderCliContract.h)
target_link_libraries(dlss5-convert PRIVATE dlss_video_player_common dlss_video_player_shipped_code)
dlss_video_player_harden(dlss5-convert)
dlss_video_player_symbols(dlss5-convert)
if(NOT DLSS_VIDEO_PLAYER_ASAN)
    target_link_options(dlss5-convert PRIVATE /DEPENDENTLOADFLAG:0x800)
endif()
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
    # Real encodes of real media end to end: about half a minute, so it is in
    # the `slow` tier the nightly and release runs add to the quick one.
    set_tests_properties(CachedExportTests PROPERTIES SKIP_RETURN_CODE 125 LABELS slow)
    add_test(NAME RuntimeLockTests COMMAND RuntimeLockTests)
    add_test(NAME RangeSelectionTests COMMAND RangeSelectionTests)
    add_test(NAME FrameIdentityTests COMMAND FrameIdentityTests)
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
    # DLSS_TEST_SOURCE_DIR: every_child_spawn_names_the_handles_it_inherits_test
    # reads the shipped sources' CreateProcessW calls.
    target_compile_definitions(PolicyTests PRIVATE YOUTUBE_RESOLVER_TESTING
        "DLSS_TEST_SOURCE_DIR=L\"${CMAKE_SOURCE_DIR}/src\"")
    add_dependencies(PolicyTests DLSSVideoPlayer)
    # Two registrations of one binary: the quick tier every pull request runs,
    # and the cases that drive real child processes for a second or more each
    # (kSlowCases), labelled `slow` so `ctest --preset quick` leaves them out.
    # The lock keeps them from running side by side under `ctest -j`: both
    # write fixtures beside the one executable.
    add_test(NAME PolicyTests COMMAND PolicyTests --tier=quick)
    add_test(NAME PolicyTestsSlow COMMAND PolicyTests --tier=slow)
    set_tests_properties(PolicyTests PolicyTestsSlow PROPERTIES RESOURCE_LOCK PolicyTestsDirectory)
    set_tests_properties(PolicyTestsSlow PROPERTIES LABELS slow)

    # dlss5-convert.exe, driven for real with the test binary standing in for
    # the player (ConvertTests.cpp).
    dlss_video_player_test(ConvertTests tests/ConvertTests.cpp tests/TestSupport.h)
    add_dependencies(ConvertTests dlss5-convert)
    add_test(NAME ConvertTests COMMAND ConvertTests "$<TARGET_FILE_DIR:dlss5-convert>")

    add_executable(ReleaseApiCompileTests tests/ReleaseApiCompileTests.cpp)
    target_link_libraries(ReleaseApiCompileTests PRIVATE dlss_video_player_common)
    add_test(NAME ReleaseApiCompileTests COMMAND ReleaseApiCompileTests)

    # The two executables that ship link the runtime statically (see
    # CMAKE_MSVC_RUNTIME_LIBRARY at the top), because no package carries the
    # Visual C++ Redistributable. This reads their import tables with the
    # toolset's own dumpbin and fails on any C or C++ runtime DLL, which is the
    # one place a flag change, an import library of the other CRT or a new
    # dependency would show before a user's machine did. Not under ASan, whose
    # instrumented executables load the sanitizer runtime by design and are
    # never shipped.
    if(NOT DLSS_VIDEO_PLAYER_ASAN)
        get_filename_component(msvc_tool_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
        find_program(DLSS_VIDEO_PLAYER_DUMPBIN dumpbin.exe HINTS "${msvc_tool_directory}" NO_DEFAULT_PATH)
        if(NOT DLSS_VIDEO_PLAYER_DUMPBIN)
            message(FATAL_ERROR "dumpbin.exe is not beside ${CMAKE_CXX_COMPILER}; ShippedRuntimeImports needs it.")
        endif()
        add_test(NAME ShippedRuntimeImports COMMAND "${CMAKE_COMMAND}"
            "-DDUMPBIN=${DLSS_VIDEO_PLAYER_DUMPBIN}"
            "-DBINARIES=$<TARGET_FILE:DLSSVideoPlayer>|$<TARGET_FILE:NeuralWorker>|$<TARGET_FILE:dlss5-convert>"
            -P "${CMAKE_SOURCE_DIR}/tests/ShippedRuntimeImports.cmake")
        set_tests_properties(ShippedRuntimeImports PROPERTIES TIMEOUT 60)
        add_test(NAME PlayerDllSearchPinned COMMAND "${CMAKE_COMMAND}"
            "-DDUMPBIN=${DLSS_VIDEO_PLAYER_DUMPBIN}"
            "-DPINNED=$<TARGET_FILE:DLSSVideoPlayer>"
            "-DUNPINNED=$<TARGET_FILE:NeuralWorker>"
            -P "${CMAKE_SOURCE_DIR}/tests/PlayerDllSearchPinned.cmake")
        set_tests_properties(PlayerDllSearchPinned PROPERTIES TIMEOUT 60)
        # P1.30: the packager refuses a dirty tree and, for a release, a HEAD
        # that is not the release tag. Needs git, which every checkout has.
        add_test(NAME PackageSourceIdentity COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/PackageSourceIdentity.ps1" -Tools "${CMAKE_SOURCE_DIR}/tools")
        set_tests_properties(PackageSourceIdentity PROPERTIES TIMEOUT 60)
        # P1.31: the complete package needs a passing hardware-suite report.
        add_test(NAME HardwareTestReport COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tests/HardwareTestReport.ps1" -Tools "${CMAKE_SOURCE_DIR}/tools")
        set_tests_properties(HardwareTestReport PROPERTIES TIMEOUT 60)
    endif()

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
    if(DLSS_VIDEO_PLAYER_HARDWARE_TESTS)
        include("${CMAKE_CURRENT_SOURCE_DIR}/tests/HardwareTests.cmake")
    endif()

    # Every test ends: a hung helper child or a deadlocked wait fails the run
    # instead of stalling it. Each budget is several times its measured Release
    # run time on the reference machine; NeuralWorkerTests sleeps a child for
    # 20 s and the hardware smokes render real frames.
    set_tests_properties(RecentMediaTests RenderSettingsTests RuntimeLockTests RangeSelectionTests
        UpscalingTests ReleaseApiCompileTests UpdateCheckTests PROPERTIES TIMEOUT 60)
    set_tests_properties(FrameIdentityTests CachedExportTests PlayerUiRegressionTests
        PROPERTIES TIMEOUT 300)
    set_tests_properties(NeuralWorkerTests PolicyTests PolicyTestsSlow NeuralPrerenderTests PROPERTIES TIMEOUT 600)

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
