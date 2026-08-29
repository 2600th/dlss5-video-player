@echo off
setlocal
cd /d "%~dp0"
call prepare_dlss5_test.bat --no-pause
if errorlevel 1 (
  echo.
  pause
  exit /b 1
)
if exist "DLSSVideoPlayer.exe" (
  set "EXE=DLSSVideoPlayer.exe"
) else (
  set "EXE=build\Release\DLSSVideoPlayer.exe"
)
if not exist "%EXE%" exit /b 1
start "" "%EXE%" --quality auto %*
