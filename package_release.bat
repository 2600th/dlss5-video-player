@echo off
setlocal EnableExtensions
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1
exit /b %ERRORLEVEL%
