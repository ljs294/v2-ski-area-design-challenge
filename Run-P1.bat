@echo off
setlocal
cd /d "%~dp0"
title Mountain Planner - Unreal P1

set "PROVIDER=sample"
set "PROVIDER_DESCRIPTION=sample terrain"
if /i "%~1"=="live" (
  set "PROVIDER=live"
  set "PROVIDER_DESCRIPTION=live terrain providers"
)

echo Launching Mountain Planner with %PROVIDER_DESCRIPTION%...
powershell -NoProfile -ExecutionPolicy Bypass -File "Tools\Build\resolve_p1_release.ps1" -Launch -Provider "%PROVIDER%" >nul 2>nul
if errorlevel 1 (
  echo No validated packaged P1 release handoff exists yet. Building it now...
  echo.
  call Build-P1-Release.bat nopause
  if errorlevel 1 exit /b 1
  powershell -NoProfile -ExecutionPolicy Bypass -File "Tools\Build\resolve_p1_release.ps1" -Launch -Provider "%PROVIDER%"
  if not errorlevel 1 goto launched
  echo.
  echo ERROR: Release assembly did not produce and launch a validated handoff.
  pause
  exit /b 1
)

:launched
endlocal
exit /b 0
