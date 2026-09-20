@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "DLSS_SDK_COMMIT=a291cc7d2cc642a51566f3dfd5376f635cd1b284"
if not defined DLSS_SDK_DIR set "DLSS_SDK_DIR=%CD%\external\DLSS"
if not defined FFMPEG_BIN_DIR set "FFMPEG_BIN_DIR=%CD%\external\ffmpeg\bin"

set "CMAKE_EXE="
set "VS_GENERATOR="
rem Visual Studio 2026 installs under its major version number - "...\Microsoft
rem Visual Studio\18\<Edition>" - and not under the year the way 2022 did, so the
rem 2026 probe looks for 18 while only the 2022 probe still uses a year.
for %%E in (Community Professional Enterprise BuildTools) do if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  set "VS_GENERATOR=Visual Studio 18 2026"
)
for %%E in (Community Professional Enterprise BuildTools) do if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\18\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  set "VS_GENERATOR=Visual Studio 18 2026"
)
for %%E in (Community Professional Enterprise BuildTools) do if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  set "VS_GENERATOR=Visual Studio 17 2022"
)
for %%E in (Community Professional Enterprise BuildTools) do if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  set "VS_GENERATOR=Visual Studio 17 2022"
)
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE (
  echo [ERROR] CMake from Visual Studio 2026 or 2022 was not found.
  exit /b 1
)
rem VS_GENERATOR is still empty only when CMake came from PATH rather than a
rem Visual Studio install, so nothing identified the toolset. Leave it empty and
rem let CMake use its own default generator, which is the newest Visual Studio
rem that CMake knows about. Naming a generator here instead is what made this
rem script ask a 2026-only machine for the 2022 v143 toolset and fail MSB8020.
rem A configured build directory is pinned to the generator that created it. That
rem pin is the best information available only when nothing else identified a
rem toolset; overriding a Visual Studio this script just found with a stale pin is
rem what turns the cache a failed configure leaves behind into the same MSB8020
rem failure on every later run - and whoever hit that failure has exactly such a
rem cache. So when the two disagree, keep what was detected and let CMake refuse
rem the mismatched cache, which names the directory to delete.
set "CACHED_GENERATOR="
if exist "build-upscaling\CMakeCache.txt" for /f "tokens=2 delims==" %%I in ('findstr /b /c:"CMAKE_GENERATOR:INTERNAL=" "build-upscaling\CMakeCache.txt"') do set "CACHED_GENERATOR=%%I"
if defined CACHED_GENERATOR (
  if not defined VS_GENERATOR (
    set "VS_GENERATOR=%CACHED_GENERATOR%"
  ) else if not "%CACHED_GENERATOR%"=="%VS_GENERATOR%" (
    echo [NOTE] build-upscaling was configured with "%CACHED_GENERATOR%", not the
    echo        detected "%VS_GENERATOR%". Configure will refuse the mismatched
    echo        cache; delete build-upscaling to build with what was detected.
  )
)
for %%I in ("%CMAKE_EXE%") do set "CTEST_EXE=%%~dpIctest.exe"
if not exist "%CTEST_EXE%" set "CTEST_EXE=ctest.exe"

if not exist "%DLSS_SDK_DIR%\include\nvsdk_ngx.h" (
  echo [ERROR] Pinned NVIDIA DLSS SDK checkout is missing: "%DLSS_SDK_DIR%"
  echo See docs\BUILDING.md. No unpinned SDK will be downloaded automatically.
  exit /b 1
)
set "ACTUAL_DLSS_COMMIT="
for /f "delims=" %%I in ('git -C "%DLSS_SDK_DIR%" rev-parse HEAD 2^>nul') do set "ACTUAL_DLSS_COMMIT=%%I"
if /i not "%ACTUAL_DLSS_COMMIT%"=="%DLSS_SDK_COMMIT%" (
  echo [ERROR] NVIDIA DLSS SDK revision mismatch.
  echo Expected: %DLSS_SDK_COMMIT%
  echo Actual  : %ACTUAL_DLSS_COMMIT%
  exit /b 1
)

if not exist "%FFMPEG_BIN_DIR%\ffmpeg.exe" (
  echo [ERROR] Verified FFmpeg input is missing: "%FFMPEG_BIN_DIR%\ffmpeg.exe"
  exit /b 1
)
if not exist "%FFMPEG_BIN_DIR%\ffprobe.exe" (
  echo [ERROR] Verified FFmpeg input is missing: "%FFMPEG_BIN_DIR%\ffprobe.exe"
  exit /b 1
)

echo [1/5] Fetching pinned UI assets...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\fetch_ui_assets.ps1
if errorlevel 1 exit /b 1

echo [2/5] Fetching pinned YouTube helpers...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\fetch_youtube_helpers.ps1
if errorlevel 1 exit /b 1

echo [3/5] Fetching and validating the locked experimental runtime...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\fetch_neural_runtime.ps1
if errorlevel 1 (
  echo [ERROR] Locked runtime could not be fetched or has drifted. See docs\BUILDING.md.
  exit /b 1
)

if defined VS_GENERATOR (
  echo [4/5] Configuring %VS_GENERATOR% x64...
  "%CMAKE_EXE%" -S . -B build-upscaling -G "%VS_GENERATOR%" -A x64 -DBUILD_TESTING=ON "-DDLSS_SDK=%DLSS_SDK_DIR%" "-DFFMPEG_STAGED_DIR=%FFMPEG_BIN_DIR%"
) else (
  rem No -A here: the platform flag is only accepted by generators that have one,
  rem and this branch runs precisely when nothing identified the generator. A
  rem Visual Studio default already targets the x64 host.
  echo [4/5] Configuring with CMake's default generator...
  "%CMAKE_EXE%" -S . -B build-upscaling -DBUILD_TESTING=ON "-DDLSS_SDK=%DLSS_SDK_DIR%" "-DFFMPEG_STAGED_DIR=%FFMPEG_BIN_DIR%"
)
if errorlevel 1 (
  echo [ERROR] Configure failed. If the generator named above is not the Visual
  echo         Studio you have installed, delete build-upscaling and run again:
  echo         a configured build directory keeps the generator that created it.
  exit /b 1
)

echo [5/5] Building and testing Release...
"%CMAKE_EXE%" --build build-upscaling --config Release --parallel
if errorlevel 1 exit /b 1
"%CTEST_EXE%" --test-dir build-upscaling -C Release --output-on-failure
if errorlevel 1 exit /b 1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\stage_runtime.ps1 -InputDirectory external\runtime -Destination build-upscaling\Release\neural-runtime
if errorlevel 1 exit /b 1
copy /y packaging\ReShade.ini build-upscaling\Release\neural-runtime\ReShade.ini >nul
if errorlevel 1 exit /b 1
copy /y packaging\ReShadePreset.ini build-upscaling\Release\neural-runtime\ReShadePreset.ini >nul
if errorlevel 1 exit /b 1
rem A single-config generator puts the binary elsewhere and ignores --config
rem Release without a word, so this line used to be printed for a path that
rem need not exist. Check before claiming it.
if not exist "build-upscaling\Release\DLSSVideoPlayer.exe" (
  echo [ERROR] build-upscaling\Release\DLSSVideoPlayer.exe was not produced.
  echo         The generator above is probably single-config ^(Ninja or NMake^),
  echo         which ignores --config Release. Delete build-upscaling and run
  echo         again from a Developer Command Prompt, or pass a Visual Studio
  echo         generator explicitly.
  exit /b 1
)
echo [OK] build-upscaling\Release\DLSSVideoPlayer.exe
echo Release packaging is a separate, allowlisted step: package_release.bat
