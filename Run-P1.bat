@echo off
setlocal
cd /d "%~dp0"
title Mountain Planner - Unreal P1

set "GAME_EXE=%CD%\release\MountainPlanner-P1-Windows\SkiAreaDesignChallenge.exe"

if not exist "%GAME_EXE%" (
  echo No packaged P1 release exists yet. Building it now...
  echo.
  call Build-P1-Release.bat nopause
  if errorlevel 1 exit /b 1
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
