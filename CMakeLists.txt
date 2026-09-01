cmake_minimum_required(VERSION 3.24)
project(DLSSVideoPlayer VERSION 0.11.0 LANGUAGES CXX RC)

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

set(DLSS_SDK "${CMAKE_SOURCE_DIR}/external/DLSS" CACHE PATH "Path to NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "NVIDIA DLSS SDK not found at ${DLSS_SDK}. Run build_windows.bat; it clones the official NVIDIA/DLSS repository automatically.")
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
    src/CompletionRegistry.h
    src/NetworkMediaTransaction.h
)

target_compile_definitions(DLSSVideoPlayer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)
target_compile_options(DLSSVideoPlayer PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
target_include_directories(DLSSVideoPlayer PRIVATE "${DLSS_SDK}/include")
target_link_directories(DLSSVideoPlayer PRIVATE "${DLSS_LIB_DIR}")
target_link_libraries(DLSSVideoPlayer PRIVATE
    d3d12 dxgi dxguid d3dcompiler
    mfplat mfreadwrite mfuuid
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi winhttp
    nvsdk_ngx_d
)

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
set(FFMPEG_STAGED_DIR "${CMAKE_SOURCE_DIR}/external/ffmpeg/bin")
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

if(BUILD_TESTING)
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
        src/VideoDecoder.cpp
        src/VideoDecoder.h
        src/CompletionRegistry.h
        src/AudioPlayer.cpp
        src/AudioPlayer.h
        src/NetworkMediaTransaction.h
    )
    target_include_directories(PolicyTests PRIVATE src)
    target_compile_definitions(PolicyTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX YOUTUBE_RESOLVER_TESTING VIDEO_DECODER_TESTING AUDIO_PLAYER_TESTING
    )
    target_compile_options(PolicyTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_libraries(PolicyTests PRIVATE dxgi shlwapi shell32 winhttp mfplat mfreadwrite mfuuid ole32 winmm)
    add_dependencies(PolicyTests DLSSVideoPlayer)
    add_test(NAME PolicyTests COMMAND PolicyTests)

    add_executable(ReleaseApiCompileTests tests/ReleaseApiCompileTests.cpp)
    target_include_directories(ReleaseApiCompileTests PRIVATE src)
    target_compile_definitions(ReleaseApiCompileTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(ReleaseApiCompileTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    add_test(NAME ReleaseApiCompileTests COMMAND ReleaseApiCompileTests)

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
