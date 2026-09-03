@echo off
rem Compatibility launcher: neural output preserves source resolution.
rem Select 2160p Super Resolution in the player DLSS menu if wanted.
setlocal
cd /d "%~dp0"
if exist "DLSSVideoPlayer.exe" (
  set "EXE=DLSSVideoPlayer.exe"
) else (
  set "EXE=build\Release\DLSSVideoPlayer.exe"
)
if not exist "%EXE%" (
  echo Build first with build_windows.bat
  pause
  exit /b 1
)
"%EXE%" %*
