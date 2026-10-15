@echo off
rem Builds the player with a manually assembled MSVC environment. vcvars64.bat
rem cannot be used in this sandbox: it calls reg.exe, which is on the host's
rem program blacklist, and without the registry it silently skips the Windows
rem SDK paths (link.exe then cannot find kernel32.lib). The paths below are the
rem ones vcvars would have injected, for this machine's toolchain.
set "PATH=F:\DevTools\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64;F:\DevTools\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;F:\DevTools\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "INCLUDE=F:\DevTools\BuildTools\VC\Tools\MSVC\14.44.35207\include;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\shared;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\winrt;C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\ucrt"
set "LIB=F:\DevTools\BuildTools\VC\Tools\MSVC\14.44.35207\lib\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64;C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64"
cd /d F:\dlss5-video\dlss5-video-amd
cmake --build build-ninja --target DLSSVideoPlayer
rem Stage the freshly built exe into run-fg (the manual run/test staging dir that
rem holds the FFX DLLs + ffmpeg + the smoke/ab python harnesses). build-fsr4.bat
rem and CMake POST_BUILD only write to build-ninja, so sync run-fg on success.
if %ERRORLEVEL% EQU 0 (
    copy /Y build-ninja\DLSSVideoPlayer.exe run-fg\
)
