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

# Source files are UTF-8 without a BOM. Do not let the Windows ANSI code page
# reinterpret UI punctuation when compiling wide string literals.
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Provide the pinned official NVIDIA/DLSS checkout documented in docs/BUILDING.md.")
endif()

set(DLSS_LIB_DIR "${DLSS_SDK}/lib/Windows_x86_64/x64")
set(DLSS_RUNTIME_REL "${DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
set(DLSS_RUNTIME_DEV "${DLSS_SDK}/lib/Windows_x86_64/dev/nvngx_dlss.dll")

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
    src/AudioPlayer.cpp
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
    src/SynchronizedPlayback.cpp
    src/SynchronizedPlayback.h
    src/NeuralSegmentIndex.h
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
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi winhttp bcrypt
    nvsdk_ngx_d
)
dlss_video_player_use_nvof(DLSSVideoPlayer)

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
set_target_properties(NeuralWorker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/neural-runtime")
add_dependencies(DLSSVideoPlayer NeuralWorker)

add_custom_command(TARGET DLSSVideoPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<IF:$<CONFIG:Debug>,${DLSS_RUNTIME_DEV},${DLSS_RUNTIME_REL}>"
        "$<TARGET_FILE_DIR:DLSSVideoPlayer>/nvngx_dlss.dll"
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
    add_executable(CachedExportTests tests/CachedExportTests.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/MediaSource.cpp)
    add_executable(RuntimeLockTests tests/RuntimeLockTests.cpp src/RuntimeLock.cpp src/NeuralReceipt.cpp
        src/NeuralPreflight.cpp src/NeuralCache.cpp)
    add_executable(RangeSelectionTests tests/RangeSelectionTests.cpp src/RangeSelection.cpp)
    # SynchronizedPlayback's DecoderFrameSource is compiled in every build now that
    # the sources are injected rather than #ifdef-ed, so a target that links the
    # playback also links the decoder behind it.
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
    target_link_libraries(CachedExportTests PRIVATE shell32 mfplat mfreadwrite mfuuid ole32)
    target_link_libraries(RuntimeLockTests PRIVATE bcrypt shell32 ole32 version)
    target_link_libraries(FrameIdentityTests PRIVATE mfplat mfreadwrite mfuuid ole32 shell32)
    add_test(NAME RecentMediaTests COMMAND RecentMediaTests)
    add_test(NAME RenderSettingsTests COMMAND RenderSettingsTests)
    add_test(NAME CachedExportTests COMMAND CachedExportTests "${FFMPEG_STAGED_DIR}")
    add_test(NAME RuntimeLockTests COMMAND RuntimeLockTests)
    add_test(NAME RangeSelectionTests COMMAND RangeSelectionTests)
    add_test(NAME FrameIdentityTests COMMAND FrameIdentityTests)
    add_executable(UpscalingGpuSmoke tests/UpscalingGpuSmoke.cpp
        src/D3D12Renderer.cpp src/DLSSBackend.cpp src/TemporalGuides.cpp src/OpticalFlowNvof.cpp
        src/VideoDecoder.cpp src/MediaSource.cpp src/RuntimePolicy.cpp)
    target_include_directories(UpscalingGpuSmoke PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(UpscalingGpuSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_directories(UpscalingGpuSmoke PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(UpscalingGpuSmoke PRIVATE ${player_libraries})
    dlss_video_player_use_nvof(UpscalingGpuSmoke)
    add_executable(MediaGpuSmoke tests/MediaGpuSmoke.cpp
        src/NeuralWorker.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/MediaSource.cpp)
    target_include_directories(MediaGpuSmoke PRIVATE src)
    target_compile_definitions(MediaGpuSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(MediaGpuSmoke PRIVATE /W4 /permissive- /EHsc)
    target_link_libraries(MediaGpuSmoke PRIVATE shell32 mfplat mfreadwrite mfuuid ole32)
    add_dependencies(MediaGpuSmoke NeuralWorker)
    add_executable(NeuralWorkerTests tests/NeuralWorkerTests.cpp src/NeuralWorker.cpp)
    target_include_directories(NeuralWorkerTests PRIVATE src)
    target_compile_definitions(NeuralWorkerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(NeuralWorkerTests PRIVATE shell32)
    add_test(NAME NeuralWorkerTests COMMAND NeuralWorkerTests)
    add_executable(UpscalingTests tests/UpscalingTests.cpp)
    target_include_directories(UpscalingTests PRIVATE src)
    target_compile_definitions(UpscalingTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
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
        src/ReShadeConfig.cpp
        src/ReShadeConfig.h
        src/YouTubeResolver.cpp
        src/YouTubeResolver.h
        src/ExampleVideos.h
        src/VideoDecoder.cpp
        src/VideoDecoder.h
        src/MediaSource.cpp
        src/CompletionRegistry.h
        src/AudioPlayer.cpp
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
        src/ReleasePackagePolicy.h
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
    target_link_libraries(PolicyTests PRIVATE d3d12 dxgi dxguid d3dcompiler shlwapi shell32 winhttp mfplat mfreadwrite mfuuid ole32 winmm nvsdk_ngx_d)
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
    # Now that the production adapters compile here, the job's stage-cost logging
    # runs too, and Log opens <module directory>/DLSSVideoPlayer.log truncating.
    # Beside the player that erases its log every ctest run; the cache-root probe
    # also stages a directory beside the executable. Give both their own place.
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
    add_test(NAME PlayerUiRegressionTests COMMAND PlayerUiRegressionTests)

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
