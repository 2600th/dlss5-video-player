@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ================================================================
echo DLSS Video Player V11 - Windows x64 D3D12 / NGX / FFmpeg
echo ================================================================

echo [0/5] Locating build tools...

set "GIT_EXE="
for /f "delims=" %%I in ('where git.exe 2^>nul') do if not defined GIT_EXE set "GIT_EXE=%%I"
if not defined GIT_EXE if exist "%ProgramFiles%\Git\cmd\git.exe" set "GIT_EXE=%ProgramFiles%\Git\cmd\git.exe"
if not defined GIT_EXE if exist "%ProgramFiles(x86)%\Git\cmd\git.exe" set "GIT_EXE=%ProgramFiles(x86)%\Git\cmd\git.exe"
if not defined GIT_EXE (
    echo [ERROR] git.exe not found. Install Git for Windows.
    pause
    exit /b 1
)
echo [INFO] Git   : "%GIT_EXE%"

set "CMAKE_EXE="
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_EXE set "CMAKE_EXE=%%I"
if not defined CMAKE_EXE if exist "%ProgramFiles%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
if not defined CMAKE_EXE if exist "%ProgramFiles(x86)%\CMake\bin\cmake.exe" set "CMAKE_EXE=%ProgramFiles(x86)%\CMake\bin\cmake.exe"
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined CMAKE_EXE if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined CMAKE_EXE if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
        if exist "%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
            set "CMAKE_EXE=%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        )
    )
)
if not defined CMAKE_EXE (
    echo.
    echo [ERROR] CMake was not found in PATH or inside Visual Studio 2022.
    echo Install "C++ CMake tools for Windows" in Visual Studio Installer.
    pause
    exit /b 1
)
echo [INFO] CMake : "%CMAKE_EXE%"

set "VS_INSTALL="
for %%E in (Community Professional Enterprise BuildTools) do (
    if not defined VS_INSTALL if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Tools\MSVC" (
        set "VS_INSTALL=%ProgramFiles%\Microsoft Visual Studio\2022\%%E"
    )
)
if defined VS_INSTALL (
    echo [INFO] VS2022: "!VS_INSTALL!"
) else (
    echo [WARN] VS2022 C++ toolset not verified; CMake will try to locate it.
)

if not exist "external\DLSS\include\nvsdk_ngx.h" (
    echo.
    echo [1/5] Cloning official NVIDIA DLSS SDK...
    if not exist "external" mkdir "external"
    "%GIT_EXE%" clone --depth 1 https://github.com/NVIDIA/DLSS.git "external\DLSS"
    if errorlevel 1 (
        echo [ERROR] Failed to clone NVIDIA/DLSS.
        pause
        exit /b 1
    )
) else (
    echo.
    echo [1/5] NVIDIA DLSS SDK already present. Updating official checkout...
    "%GIT_EXE%" -C "external\DLSS" pull --ff-only
    if errorlevel 1 (
        echo [WARN] Could not update NVIDIA/DLSS; continuing with the existing checkout.
    )
)

echo.
echo [2/5] Preparing FFmpeg universal video decoder...
set "FFMPEG_DIR=external\ffmpeg\bin"
if exist "%FFMPEG_DIR%\ffmpeg.exe" if exist "%FFMPEG_DIR%\ffprobe.exe" goto ffmpeg_ready

if not exist "external" mkdir "external"
if not exist "external\ffmpeg" mkdir "external\ffmpeg"
if not exist "%FFMPEG_DIR%" mkdir "%FFMPEG_DIR%"

rem First reuse an FFmpeg already installed on Windows, if present.
set "SYSTEM_FFMPEG="
set "SYSTEM_FFPROBE="
for /f "delims=" %%I in ('where ffmpeg.exe 2^>nul') do if not defined SYSTEM_FFMPEG set "SYSTEM_FFMPEG=%%I"
for /f "delims=" %%I in ('where ffprobe.exe 2^>nul') do if not defined SYSTEM_FFPROBE set "SYSTEM_FFPROBE=%%I"
if defined SYSTEM_FFMPEG if defined SYSTEM_FFPROBE (
    echo [INFO] Reusing FFmpeg already installed on this PC.
    copy /y "!SYSTEM_FFMPEG!" "%FFMPEG_DIR%\ffmpeg.exe" >nul
    copy /y "!SYSTEM_FFPROBE!" "%FFMPEG_DIR%\ffprobe.exe" >nul
    goto ffmpeg_ready
)

set "FFMPEG_ZIP=external\ffmpeg-release-essentials.zip"
set "FFMPEG_TMP=external\ffmpeg_unpack"
set "FFMPEG_URL=https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
if exist "%FFMPEG_TMP%" rmdir /s /q "%FFMPEG_TMP%"

rem A cancelled V3 download can leave a partial ZIP. Validate it before reuse.
if exist "%FFMPEG_ZIP%" (
    echo [INFO] Checking existing FFmpeg archive...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { Add-Type -AssemblyName System.IO.Compression.FileSystem; $z=[IO.Compression.ZipFile]::OpenRead((Resolve-Path '%FFMPEG_ZIP%')); $ok=($z.Entries.Count -gt 2); $z.Dispose(); if($ok){exit 0}else{exit 1} } catch { exit 1 }"
    if errorlevel 1 (
        echo [WARN] Existing FFmpeg ZIP is incomplete/corrupt. Deleting it.
        del /f /q "%FFMPEG_ZIP%" >nul 2>nul
    ) else (
        echo [INFO] Existing FFmpeg ZIP is valid; download skipped.
    )
)

if not exist "%FFMPEG_ZIP%" (
    echo [INFO] Downloading FFmpeg Essentials with visible progress...
    set "CURL_EXE="
    for /f "delims=" %%I in ('where curl.exe 2^>nul') do if not defined CURL_EXE set "CURL_EXE=%%I"
    if not defined CURL_EXE if exist "%SystemRoot%\System32\curl.exe" set "CURL_EXE=%SystemRoot%\System32\curl.exe"
    if defined CURL_EXE (
        "!CURL_EXE!" -L --fail --retry 3 --retry-delay 2 --connect-timeout 20 --progress-bar -o "%FFMPEG_ZIP%" "%FFMPEG_URL%"
        if errorlevel 1 (
            echo [ERROR] FFmpeg download failed with curl.
            del /f /q "%FFMPEG_ZIP%" >nul 2>nul
            pause
            exit /b 1
        )
    ) else (
        echo [WARN] curl.exe was not found. Falling back to PowerShell.
        powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='Continue'; try { Invoke-WebRequest -UseBasicParsing -Uri '%FFMPEG_URL%' -OutFile '%FFMPEG_ZIP%'; exit 0 } catch { Write-Host $_.Exception.Message; exit 1 }"
        if errorlevel 1 (
            echo [ERROR] FFmpeg download failed.
            del /f /q "%FFMPEG_ZIP%" >nul 2>nul
            pause
            exit /b 1
        )
    )
)

if not exist "%FFMPEG_ZIP%" (
    echo [ERROR] FFmpeg ZIP is missing after download.
    pause
    exit /b 1
)

echo [INFO] Extracting FFmpeg...
mkdir "%FFMPEG_TMP%" >nul 2>nul
set "TAR_EXE="
for /f "delims=" %%I in ('where tar.exe 2^>nul') do if not defined TAR_EXE set "TAR_EXE=%%I"
if defined TAR_EXE (
    "!TAR_EXE!" -xf "%FFMPEG_ZIP%" -C "%FFMPEG_TMP%"
    if errorlevel 1 set "TAR_EXE="
)
if not defined TAR_EXE (
    if exist "%FFMPEG_TMP%" rmdir /s /q "%FFMPEG_TMP%"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "try { Expand-Archive -LiteralPath '%FFMPEG_ZIP%' -DestinationPath '%FFMPEG_TMP%' -Force; exit 0 } catch { Write-Host $_.Exception.Message; exit 1 }"
    if errorlevel 1 (
        echo [ERROR] Could not extract FFmpeg ZIP. Delete "%FFMPEG_ZIP%" and retry.
        pause
        exit /b 1
    )
)

set "FOUND_FFMPEG="
set "FOUND_FFPROBE="
for /f "delims=" %%I in ('where /r "%FFMPEG_TMP%" ffmpeg.exe 2^>nul') do if not defined FOUND_FFMPEG set "FOUND_FFMPEG=%%I"
for /f "delims=" %%I in ('where /r "%FFMPEG_TMP%" ffprobe.exe 2^>nul') do if not defined FOUND_FFPROBE set "FOUND_FFPROBE=%%I"
if not defined FOUND_FFMPEG (
    echo [ERROR] ffmpeg.exe was not found inside the downloaded archive.
    pause
    exit /b 1
)
if not defined FOUND_FFPROBE (
    echo [ERROR] ffprobe.exe was not found inside the downloaded archive.
    pause
    exit /b 1
)
copy /y "!FOUND_FFMPEG!" "%FFMPEG_DIR%\ffmpeg.exe" >nul
copy /y "!FOUND_FFPROBE!" "%FFMPEG_DIR%\ffprobe.exe" >nul
if exist "%FFMPEG_TMP%" rmdir /s /q "%FFMPEG_TMP%"

:ffmpeg_ready
if not exist "%FFMPEG_DIR%\ffmpeg.exe" (
    echo [ERROR] FFmpeg staging failed.
    pause
    exit /b 1
)
echo [INFO] FFmpeg ready: %FFMPEG_DIR%\ffmpeg.exe

echo.
echo [3/5] Configuring Visual Studio 2022 x64...
"%CMAKE_EXE%" -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo [ERROR] CMake configure failed.
    pause
    exit /b 1
)

echo.
echo [4/5] Building Release...
"%CMAKE_EXE%" --build build --config Release --parallel
if errorlevel 1 (
    echo [ERROR] Build failed. Send the compiler output above.
    pause
    exit /b 1
)

echo.
echo [5/5] Staging runtime files...
if not exist "build\Release\DLSSVideoPlayer.exe" (
    echo [ERROR] Build reported success but the EXE was not found.
    pause
    exit /b 1
)
copy /y "%FFMPEG_DIR%\ffmpeg.exe" "build\Release\ffmpeg.exe" >nul
copy /y "%FFMPEG_DIR%\ffprobe.exe" "build\Release\ffprobe.exe" >nul
if errorlevel 1 (
    echo [ERROR] Could not copy FFmpeg beside DLSSVideoPlayer.exe.
    pause
    exit /b 1
)

rem Convenience: stage an experimental RenoDX/Streamline pack if the user put it
rem beside this BAT or extracted it into .\streamline. A user-supplied nvngx_dlss.dll
rem intentionally overrides the official SR DLL in Release so a matched DLSS/DLSSNR pair
rem from the experimental pack can be tested together.
for %%D in (. streamline Streamline) do (
    for %%N in (renodx-dlss5.addon64 nvngx_dlssnr.dll nvngx_dlss.dll nvngx_dlssg.dll nvngx_dlssd.dll dxgi.dll ReShade.ini ReShadePreset.ini) do (
        if exist "%%D\%%N" (
            copy /y "%%D\%%N" "build\Release\%%N" >nul
            echo [INFO] Staged optional runtime: %%D\%%N
        )
    )
    for %%F in ("%%D\sl.*.dll") do if exist "%%~fF" (
        copy /y "%%~fF" "build\Release\%%~nxF" >nul
        echo [INFO] Staged Streamline runtime: %%~nxF
    )
    for %%F in ("%%D\*.license.txt") do if exist "%%~fF" copy /y "%%~fF" "build\Release\%%~nxF" >nul
)
copy /y "prepare_dlss5_test.bat" "build\Release\prepare_dlss5_test.bat" >nul
copy /y "inspect_dlssnr.ps1" "build\Release\inspect_dlssnr.ps1" >nul
copy /y "run_dlss5_test.bat" "build\Release\run_dlss5_test.bat" >nul
copy /y "run_4k_auto.bat" "build\Release\run_4k_auto.bat" >nul
copy /y "run_4k_quality.bat" "build\Release\run_4k_quality.bat" >nul
copy /y "DLSS5_CHECKLIST.txt" "build\Release\DLSS5_CHECKLIST.txt" >nul
copy /y "LICENSE" "build\Release\LICENSE" >nul
copy /y "README.md" "build\Release\README.md" >nul
copy /y "CHANGELOG.md" "build\Release\CHANGELOG.md" >nul
copy /y "THIRD_PARTY.md" "build\Release\THIRD_PARTY.md" >nul
if not exist "build\Release\docs" mkdir "build\Release\docs"
xcopy /e /i /y "docs\*" "build\Release\docs\" >nul
if not exist "build\Release\languages" mkdir "build\Release\languages"
xcopy /e /i /y "languages\*" "build\Release\languages\" >nul

echo.
echo ================================================================
echo [OK] build\Release\DLSSVideoPlayer.exe
echo [OK] build\Release\ffmpeg.exe
echo [OK] build\Release\ffprobe.exe
echo ================================================================
echo.
echo V11 adds live post-DLSS image adjustments, paused-frame ReShade presentation,
echo and render-surface flicker fixes while retaining the realtime three-frame D3D12 pipeline.
echo Media Foundation remains only as a fallback decoder.
echo.
echo Native DLSS SR is now self-contained in build\Release.
echo For experimental DLSS 5 Neural Rendering, run prepare_dlss5_test.bat
echo after placing the complete matching RenoDX/ReShade/DLSSNR/Streamline test pack beside this BAT
echo or inside the .\streamline folder. The BAT stages supported files into build\Release.
echo.
pause
