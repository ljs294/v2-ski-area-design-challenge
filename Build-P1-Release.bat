@echo off
setlocal
cd /d "%~dp0"
title Build Mountain Planner P1 Release

echo ============================================
echo   Build Mountain Planner P1 Release
echo ============================================
echo.

set "PYTHON_COMMAND=python"
where python >nul 2>nul
if errorlevel 1 (
  where py >nul 2>nul
  if errorlevel 1 (
    echo ERROR: Python 3 was not found on PATH.
    echo Python is needed only to build the release, not to run it.
    echo.
    pause
    exit /b 1
  )
  set "PYTHON_COMMAND=py -3"
)

echo [1/5] Running the focused GeoTIFF crash regression...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py tiff
if errorlevel 1 (
  echo.
  echo ERROR: Focused GeoTIFF regression failed. The release was not built.
  echo.
  pause
  exit /b 1
)

echo.
echo [2/5] Running the complete P1 preparation automation group...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py automation
if errorlevel 1 (
  echo.
  echo ERROR: P1 automation failed. The release was not built.
  echo.
  pause
  exit /b 1
)

echo.
echo [3/5] Building the self-contained Shipping package...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py package --configuration Shipping
if errorlevel 1 (
  echo.
  echo ERROR: Shipping packaging failed. Review the run-output path above.
  echo.
  pause
  exit /b 1
)

echo.
echo [4/5] Running the GeoTIFF regression through the Shipping executable...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py smoke --configuration Shipping --scenario geotiff-regression
if errorlevel 1 (
  echo.
  echo ERROR: The packaged GeoTIFF regression failed. The release was not assembled.
  echo.
  pause
  exit /b 1
)

echo.
echo [5/5] Creating the tester folder and ZIP...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "Tools\Build\assemble_p1_release.ps1"
if errorlevel 1 (
  echo.
  echo ERROR: Release assembly failed. Review the messages above.
  echo.
  pause
  exit /b 1
)

echo.
echo Release ready:
echo   release\MountainPlanner-P1-Windows\
echo   release\MountainPlanner-P1-Windows.zip
echo.
echo Give testers the ZIP. After extracting it, they can double-click
echo START MOUNTAIN PLANNER.bat or START SAMPLE TERRAIN.bat.
echo.
if /i not "%~1"=="nopause" pause
endlocal
exit /b 0
