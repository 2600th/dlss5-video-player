@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "DLSS_SDK_COMMIT=a291cc7d2cc642a51566f3dfd5376f635cd1b284"
if not defined DLSS_SDK_DIR set "DLSS_SDK_DIR=%CD%\external\DLSS"
if not defined FFMPEG_BIN_DIR set "FFMPEG_BIN_DIR=%CD%\external\ffmpeg\bin"

set "CMAKE_EXE="
set "VS_GENERATOR="
for %%E in (Community Professional Enterprise BuildTools) do if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2026\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2026\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  set "VS_GENERATOR=Visual Studio 18 2026"
)
for %%E in (Community Professional Enterprise BuildTools) do if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2026\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
  set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2026\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
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
rem Only reached when CMake came from PATH rather than a Visual Studio install, so
rem nothing identified the toolset: assume the older generator, which the newer
rem Visual Studio can still build, instead of one an older CMake does not know.
if not defined VS_GENERATOR set "VS_GENERATOR=Visual Studio 17 2022"
rem A configured build directory is pinned to the generator that created it. Reuse
rem it so installing a newer Visual Studio beside the old one does not make CMake
rem refuse the existing cache.
if exist "build-upscaling\CMakeCache.txt" for /f "tokens=2 delims==" %%I in ('findstr /b /c:"CMAKE_GENERATOR:INTERNAL=" "build-upscaling\CMakeCache.txt"') do set "VS_GENERATOR=%%I"
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

echo [4/5] Configuring %VS_GENERATOR% x64...
"%CMAKE_EXE%" -S . -B build-upscaling -G "%VS_GENERATOR%" -A x64 -DBUILD_TESTING=ON "-DDLSS_SDK=%DLSS_SDK_DIR%" "-DFFMPEG_STAGED_DIR=%FFMPEG_BIN_DIR%"
if errorlevel 1 exit /b 1

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
echo [OK] build-upscaling\Release\DLSSVideoPlayer.exe
echo Release packaging is a separate, allowlisted step: package_release.bat
