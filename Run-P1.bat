@echo off
setlocal
cd /d "%~dp0"
title Mountain Planner - Unreal P1

set "GAME_EXE="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -File "Tools\Build\resolve_p1_release.ps1" 2^>nul`) do set "GAME_EXE=%%I"

if not defined GAME_EXE (
  echo No validated packaged P1 release handoff exists yet. Building it now...
  echo.
  call Build-P1-Release.bat nopause
  if errorlevel 1 exit /b 1
  for /f "usebackq delims=" %%I in (`powershell -NoProfile -ExecutionPolicy Bypass -File "Tools\Build\resolve_p1_release.ps1" 2^>nul`) do set "GAME_EXE=%%I"
)

if not defined GAME_EXE (
  echo ERROR: Release assembly did not produce a validated launcher handoff.
  pause
  exit /b 1
)

set "PROVIDER_ARGUMENT=-SkiUseFixtureProvider"
set "PROVIDER_DESCRIPTION=sample terrain"
if /i "%~1"=="live" (
  set "PROVIDER_ARGUMENT="
  set "PROVIDER_DESCRIPTION=live terrain providers"
)

echo Launching Mountain Planner with %PROVIDER_DESCRIPTION%...
start "Mountain Planner P1" "%GAME_EXE%" %PROVIDER_ARGUMENT%
if errorlevel 1 (
  echo.
  echo ERROR: Windows could not launch the packaged application.
  pause
  exit /b 1
)

endlocal
exit /b 0
