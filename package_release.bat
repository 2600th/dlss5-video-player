@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "VERSION=0.11.0"
set "SRC=build\Release"
set "STAGE=dist\DLSSVideoPlayer-v%VERSION%-win64"
set "ZIP=dist\DLSSVideoPlayer-v%VERSION%-win64.zip"

if not exist "%SRC%\DLSSVideoPlayer.exe" (
  echo [ERROR] Build first with build_windows.bat
  exit /b 1
)

if exist "%STAGE%" rmdir /s /q "%STAGE%"
if not exist "dist" mkdir "dist"
mkdir "%STAGE%"

copy /y "%SRC%\DLSSVideoPlayer.exe" "%STAGE%\DLSSVideoPlayer.exe" >nul
copy /y "%SRC%\ffmpeg.exe" "%STAGE%\ffmpeg.exe" >nul
copy /y "%SRC%\ffprobe.exe" "%STAGE%\ffprobe.exe" >nul

rem Always package the official DLSS SR DLL, not a locally staged experimental replacement.
if exist "external\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll" (
  copy /y "external\DLSS\lib\Windows_x86_64\rel\nvngx_dlss.dll" "%STAGE%\nvngx_dlss.dll" >nul
) else (
  copy /y "%SRC%\nvngx_dlss.dll" "%STAGE%\nvngx_dlss.dll" >nul
)

xcopy /e /i /y "languages\*" "%STAGE%\languages\" >nul
xcopy /e /i /y "docs\*" "%STAGE%\docs\" >nul
copy /y "README.md" "%STAGE%\README.md" >nul
copy /y "LICENSE" "%STAGE%\LICENSE" >nul
copy /y "THIRD_PARTY.md" "%STAGE%\THIRD_PARTY.md" >nul
copy /y "CHANGELOG.md" "%STAGE%\CHANGELOG.md" >nul
copy /y "prepare_dlss5_test.bat" "%STAGE%\prepare_dlss5_test.bat" >nul
copy /y "inspect_dlssnr.ps1" "%STAGE%\inspect_dlssnr.ps1" >nul
copy /y "DLSS5_CHECKLIST.txt" "%STAGE%\DLSS5_CHECKLIST.txt" >nul
copy /y "run_dlss5_test.bat" "%STAGE%\run_dlss5_test.bat" >nul
copy /y "run_4k_auto.bat" "%STAGE%\run_4k_auto.bat" >nul
copy /y "run_4k_quality.bat" "%STAGE%\run_4k_quality.bat" >nul

if exist "%ZIP%" del /f /q "%ZIP%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath '%ZIP%' -CompressionLevel Optimal"
if errorlevel 1 exit /b 1

echo [OK] %ZIP%
echo Experimental DLSS 5 / RenoDX / ReShade files are intentionally NOT bundled.
