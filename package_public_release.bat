@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\package_release.ps1" -PublicCore
exit /b %ERRORLEVEL%
