@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if exist "DLSSVideoPlayer.exe" (
  set "OUT=."
) else (
  set "OUT=build\Release"
)
set "EXE=%OUT%\DLSSVideoPlayer.exe"
set "NOPAUSE=0"
if /I "%~1"=="--no-pause" set "NOPAUSE=1"

if not exist "%EXE%" (
  echo [ERROR] %EXE% not found. Run build_windows.bat first.
  if "%NOPAUSE%"=="0" pause
  exit /b 1
)

echo ================================================================
echo DLSS Video Player V11 - DLSS / experimental DLSS 5 readiness
echo ================================================================

rem If the user dropped/extracted the experimental pack in the project root or a
rem .\streamline folder, stage the full set. This includes an optional matching
rem nvngx_dlss.dll; if supplied, it deliberately overrides the official copy for the test.
if /I not "%OUT%"=="." (
  for %%D in (. streamline Streamline) do (
    for %%N in (renodx-dlss5.addon64 nvngx_dlssnr.dll nvngx_dlss.dll nvngx_dlssg.dll nvngx_dlssd.dll dxgi.dll ReShade.ini ReShadePreset.ini) do (
      if exist "%%D\%%N" (
        copy /y "%%D\%%N" "%OUT%\%%N" >nul
        echo [STAGED] %%D\%%N
      )
    )
    for %%F in ("%%D\sl.*.dll") do if exist "%%~fF" (
      copy /y "%%~fF" "%OUT%\%%~nxF" >nul
      echo [STAGED] %%D\%%~nxF
    )
    for %%F in ("%%D\*.license.txt") do if exist "%%~fF" copy /y "%%~fF" "%OUT%\%%~nxF" >nul
  )
)

set "FAIL=0"
if exist "%OUT%\nvngx_dlss.dll" (echo [OK] DLSS SR runtime: nvngx_dlss.dll) else (echo [MISSING] nvngx_dlss.dll ^(run build_windows.bat^) & set "FAIL=1")
if exist "%OUT%\ffmpeg.exe" (echo [OK] ffmpeg.exe) else (echo [MISSING] ffmpeg.exe & set "FAIL=1")
if exist "%OUT%\ffprobe.exe" (echo [OK] ffprobe.exe) else (echo [MISSING] ffprobe.exe & set "FAIL=1")

echo.
echo Experimental DLSS 5 Neural Rendering layer:
if exist "%OUT%\renodx-dlss5.addon64" (echo [OK] renodx-dlss5.addon64) else (echo [NEEDED FOR NR] renodx-dlss5.addon64 ^(MediaFire package: https://app.mediafire.com/folder/sa9zioqbixj7e^))
if exist "%OUT%\nvngx_dlssnr.dll" (
  echo [OK] nvngx_dlssnr.dll
  if exist "inspect_dlssnr.ps1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "inspect_dlssnr.ps1" -Path "%OUT%\nvngx_dlssnr.dll"
    set "NR_SIG=!ERRORLEVEL!"
  ) else if exist "%OUT%\inspect_dlssnr.ps1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%OUT%\inspect_dlssnr.ps1" -Path "%OUT%\nvngx_dlssnr.dll"
    set "NR_SIG=!ERRORLEVEL!"
  ) else (
    powershell.exe -NoProfile -Command "$p=(Resolve-Path '%OUT%\nvngx_dlssnr.dll').Path; $v=[Diagnostics.FileVersionInfo]::GetVersionInfo($p); Write-Host ('     File version: '+$v.FileVersion); $s=Get-AuthenticodeSignature $p; Write-Host ('     Signature: '+$s.Status)" 2>nul
  )
  if defined NR_SIG if !NR_SIG! GEQ 2 echo [WARN] Neural Rendering DLL integrity/signature needs attention before blaming the player.
) else (
  echo [NEEDED FOR NR] nvngx_dlssnr.dll - get the matching experimental package from https://app.mediafire.com/folder/sa9zioqbixj7e
)
if exist "%OUT%\dxgi.dll" (echo [OK] ReShade proxy dxgi.dll) else (echo [NEEDED FOR ADD-ON] Install ReShade with add-on support into %EXE%)
if exist "%OUT%\sl.interposer.dll" (echo [OK] Streamline: sl.interposer.dll) else (echo [INFO] sl.interposer.dll not staged - the current experimental pack may require the full Streamline set.)
if exist "%OUT%\sl.common.dll" (echo [OK] Streamline: sl.common.dll) else (echo [INFO] sl.common.dll not staged.)
if exist "%OUT%\sl.dlss_nr.dll" (echo [OK] Streamline NR plugin: sl.dlss_nr.dll) else (echo [INFO] sl.dlss_nr.dll not staged - use the complete matching experimental Streamline pack if your RenoDX build expects it.)

if exist "%OUT%\renodx-dlss5.addon64" if exist "%OUT%\nvngx_dlssnr.dll" if exist "%OUT%\dxgi.dll" (
  echo.
  echo [OK] Core experimental NR files are staged beside the player.
) else (
  echo.
  echo [INFO] Native DLSS SR is still usable. Neural Rendering needs the core items above.
)

echo.
echo [NOTE] First ReShade run: press Home and verify renodx-dlss5.addon64 is enabled under Add-ons.

echo.
echo V11 internal DLSS contract supplied every frame:
echo   Color  : linear FP16, render resolution
echo   Motion : R16G16_FLOAT optical flow, current-to-previous, pixel units
echo   Depth  : one R32_TYPELESS resource, D32_FLOAT DSV + R32_FLOAT SRV, passed directly to NGX
echo   Masks  : R8_UNORM BiasCurrentColor + Disocclusion + Responsivity hints
echo   Expand : compact RGBA32F guide grid -^> full-res MV/bias + direct SV_Depth
echo   Output : FP16 UAV, target resolution
echo   Timing : Halton jitter + Reset + frame delta + MV scale
echo   NGX API: RAW CreateFeature + EvaluateFeature_C ^(legacy EvaluateFeature fallback^)
echo.
echo Diagnostics in player:
echo   MV button / 3       = visualize generated motion vectors
echo   Depth button / 4    = visualize video depth proxy
echo   Mask button / 5     = visualize the shared temporal uncertainty mask
echo   F6 Re-hook          = release/create NGX feature again
echo   D                    = toggle DLSS
echo.
echo In RenoDX, a healthy capture should show CreateFeature ^> 0 and
echo EvaluateFeature_C/evaluations increasing, with non-zero guide/output dimensions.

if "%FAIL%"=="1" (
  echo.
  echo [ERROR] Base runtime incomplete.
  if "%NOPAUSE%"=="0" pause
  exit /b 1
)

if "%NOPAUSE%"=="0" pause
exit /b 0
