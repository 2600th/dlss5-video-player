cmake_minimum_required(VERSION 3.24)
project(DLSSVideoPlayer VERSION 0.11.0 LANGUAGES CXX)

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
    src/AppMenu.cpp
    src/AppMenu.h
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
    ole32 shell32 shlwapi comdlg32 comctl32 user32 gdi32 winmm dwmapi
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
        src/RuntimePolicy.cpp
        src/RuntimePolicy.h
        src/ReShadeConfig.cpp
        src/ReShadeConfig.h
    )
    target_include_directories(PolicyTests PRIVATE src)
    target_compile_definitions(PolicyTests PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    target_compile_options(PolicyTests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
    target_link_libraries(PolicyTests PRIVATE dxgi shlwapi shell32)
    add_dependencies(PolicyTests DLSSVideoPlayer)
    add_test(NAME PolicyTests COMMAND PolicyTests)
endif()
