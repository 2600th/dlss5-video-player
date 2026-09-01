@echo off
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
"%EXE%" --output 3840x2160 --quality auto %*
