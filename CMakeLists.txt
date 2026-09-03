cmake_minimum_required(VERSION 3.24)
project(DLSSVideoPlayer VERSION 0.14.0 LANGUAGES CXX RC)

include(CTest)

if(NOT WIN32)
    message(FATAL_ERROR "DLSSVideoPlayer currently targets Windows x64 / D3D12 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "Use Visual Studio 2022 / MSVC for this build.")
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

add_executable(DLSSVideoPlayer WIN32
    src/main.cpp
    src/resources.h
    src/resources.rc
    src/AppMenu.cpp
    src/AppMenu.h
    src/UiLayout.cpp
    src/UiLayout.h
    src/UiResources.cpp
    src/UiResources.h
    src/VideoDecoder.cpp
    src/VideoDecoder.h
    src/DLSSBackend.cpp
    src/DLSSBackend.h
    src/D3D12Renderer.cpp
    src/D3D12Renderer.h
    src/D3D12FenceWait.h
    src/TemporalGuides.cpp
    src/TemporalGuides.h
    src/AudioPlayer.cpp
    src/AudioPlayer.h
    src/Log.h
    src/Localization.h
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
    src/OfflineNeuralRenderer.h
    src/NeuralWorker.cpp
    src/NeuralWorker.h
    src/SynchronizedPlayback.cpp
    src/SynchronizedPlayback.h
)

target_compile_definitions(DLSSVideoPlayer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}"
)
target_compile_options(DLSSVideoPlayer PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(DLSSVideoPlayer PRIVATE "${DLSS_SDK}/include")
target_link_directories(DLSSVideoPlayer PRIVATE "${DLSS_LIB_DIR}")
target_link_libraries(DLSSVideoPlayer PRIVATE
    d3d12 dxgi dxguid d3dcompiler
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi winhttp bcrypt
    nvsdk_ngx_d
)

# Only the offline helper loads the experimental proxy/add-on. The player root
# stays hook-free for its independent runtime Super Resolution feature.
add_executable(NeuralWorker src/NeuralWorkerMain.cpp src/NeuralWorker.cpp
    src/OfflineNeuralRenderer.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp
    src/D3D12Renderer.cpp src/DLSSBackend.cpp src/TemporalGuides.cpp
    src/RuntimePolicy.cpp src/ReShadeConfig.cpp src/UiLayout.cpp)
target_compile_definitions(NeuralWorker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(NeuralWorker PRIVATE "${DLSS_SDK}/include")
target_compile_options(NeuralWorker PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_link_directories(NeuralWorker PRIVATE "${DLSS_LIB_DIR}")
get_target_property(player_libraries DLSSVideoPlayer LINK_LIBRARIES)
target_link_libraries(NeuralWorker PRIVATE ${player_libraries})
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
    add_executable(RenderSettingsTests tests/RenderSettingsTests.cpp src/ReShadeConfig.cpp src/NeuralCache.cpp)
    add_executable(CachedExportTests tests/CachedExportTests.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/UiLayout.cpp)
    foreach(test_target RecentMediaTests RenderSettingsTests CachedExportTests)
        target_include_directories(${test_target} PRIVATE src)
        target_compile_definitions(${test_target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_compile_options(${test_target} PRIVATE /W4 /permissive- /EHsc)
    endforeach()
    target_link_libraries(RecentMediaTests PRIVATE user32)
    target_link_libraries(RenderSettingsTests PRIVATE bcrypt shell32 ole32)
    target_link_libraries(CachedExportTests PRIVATE shell32 mfplat mfreadwrite mfuuid ole32)
    add_test(NAME RecentMediaTests COMMAND RecentMediaTests)
    add_test(NAME RenderSettingsTests COMMAND RenderSettingsTests)
    add_test(NAME CachedExportTests COMMAND CachedExportTests "${FFMPEG_STAGED_DIR}")
    add_executable(UpscalingGpuSmoke tests/UpscalingGpuSmoke.cpp
        src/D3D12Renderer.cpp src/DLSSBackend.cpp src/TemporalGuides.cpp
        src/VideoDecoder.cpp src/RuntimePolicy.cpp src/UiLayout.cpp)
    target_include_directories(UpscalingGpuSmoke PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(UpscalingGpuSmoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_directories(UpscalingGpuSmoke PRIVATE "${DLSS_LIB_DIR}")
    target_link_libraries(UpscalingGpuSmoke PRIVATE ${player_libraries})
    add_executable(MediaGpuSmoke tests/MediaGpuSmoke.cpp
        src/NeuralWorker.cpp src/MediaPipeline.cpp src/VideoDecoder.cpp src/UiLayout.cpp)
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
    add_test(NAME UpscalingTests COMMAND UpscalingTests)
    add_executable(PolicyTests
        tests/PolicyTests.cpp
        tests/TestSupport.h
        src/AppMenu.h
        src/AppMenu.cpp
        src/UiLayout.cpp
        src/UiLayout.h
        src/UiResources.cpp
        src/UiResources.h
        src/RuntimePolicy.cpp
        src/RuntimePolicy.h
        src/ReShadeConfig.cpp
        src/ReShadeConfig.h
        src/YouTubeResolver.cpp
        src/YouTubeResolver.h
        src/ExampleVideos.h
        src/VideoDecoder.cpp
        src/VideoDecoder.h
        src/CompletionRegistry.h
        src/AudioPlayer.cpp
        src/AudioPlayer.h
        src/NetworkMediaTransaction.h
        src/D3D12FenceWait.h
        src/D3D12Renderer.cpp
        src/D3D12Renderer.h
        src/ReleasePackagePolicy.h
        src/DLSSBackend.cpp
        src/DLSSBackend.h
    )
    target_include_directories(PolicyTests PRIVATE src "${DLSS_SDK}/include")
    target_compile_definitions(PolicyTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX YOUTUBE_RESOLVER_TESTING VIDEO_DECODER_TESTING AUDIO_PLAYER_TESTING D3D12_RENDERER_TESTING
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
        src/MediaPipeline.cpp
        src/MediaPipeline.h
        src/OfflineNeuralRenderer.cpp
        src/OfflineNeuralRenderer.h
        src/SynchronizedPlayback.cpp
        src/SynchronizedPlayback.h
    )
    target_include_directories(NeuralPrerenderTests PRIVATE src)
    target_compile_definitions(NeuralPrerenderTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        OFFLINE_NEURAL_RENDERER_TESTING
        SYNCHRONIZED_PLAYBACK_TESTING
        DLSS_VIDEO_PLAYER_VERSION="${PROJECT_VERSION}"
    )
    target_compile_options(NeuralPrerenderTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_libraries(NeuralPrerenderTests PRIVATE bcrypt shell32)
    add_test(NAME NeuralPrerenderTests COMMAND NeuralPrerenderTests)

    # Exercise the real player completion handlers and text submitted to Win32.
    get_target_property(player_ui_sources DLSSVideoPlayer SOURCES)
    list(REMOVE_ITEM player_ui_sources src/main.cpp)
    add_executable(PlayerUiRegressionTests tests/PlayerUiRegressionTests.cpp ${player_ui_sources})
    target_include_directories(PlayerUiRegressionTests PRIVATE src "${DLSS_SDK}/include")
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
