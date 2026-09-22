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
    if /i not "%~1"=="nopause" pause
    exit /b 1
  )
  set "PYTHON_COMMAND=py -3"
)

set "SKI_P1_RELEASE_FREEZE=test-results\p1\release-freeze.json"
echo [1/10] Freezing the release-wide source identity...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py freeze
if errorlevel 1 (
  echo.
  echo ERROR: The release source could not be frozen.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [2/10] Running the focused GeoTIFF crash regression...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py tiff
if errorlevel 1 (
  echo.
  echo ERROR: Focused GeoTIFF regression failed. The release was not built.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [3/10] Running the focused acquisition and retry regression...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py acquisition
if errorlevel 1 (
  echo.
  echo ERROR: Focused acquisition regression failed. The release was not built.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [4/10] Running the focused responsive UI regression...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py ui
if errorlevel 1 (
  echo.
  echo ERROR: Focused UI regression failed. The release was not built.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [5/10] Running the complete P1 automation group...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py automation
if errorlevel 1 (
  echo.
  echo ERROR: P1 automation failed. The release was not built.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [6/10] Building the self-contained Shipping package...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py package --configuration Shipping
if errorlevel 1 (
  echo.
  echo ERROR: Shipping packaging failed. Review the run-output path above.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [7/10] Running the GeoTIFF regression through the Shipping executable...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py smoke --configuration Shipping --scenario geotiff-regression
if errorlevel 1 (
  echo.
  echo ERROR: The packaged GeoTIFF regression failed. The release was not assembled.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [8/10] Running the acquisition-policy regression through Shipping...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py smoke --configuration Shipping --scenario acquisition-regression
if errorlevel 1 (
  echo.
  echo ERROR: The packaged acquisition-policy regression failed. The release was not assembled.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [9/10] Running the responsive UI regression through Shipping...
echo.
call %PYTHON_COMMAND% Tools\Build\p1.py smoke --configuration Shipping --scenario ui-layout
if errorlevel 1 (
  echo.
  echo ERROR: The packaged UI-layout regression failed. The release was not assembled.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo [10/10] Rechecking the source freeze and creating the tester folder and ZIP...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "Tools\Build\assemble_p1_release.ps1"
if errorlevel 1 (
  echo.
  echo ERROR: Release assembly failed. Review the messages above.
  echo.
  if /i not "%~1"=="nopause" pause
  exit /b 1
)

echo.
echo Release ready:
echo   See the versioned Folder and Zip paths printed above.
echo   Existing tester builds are left intact so a running build cannot block assembly.
echo.
echo Give testers the ZIP. After extracting it, they can double-click
echo START MOUNTAIN PLANNER.bat or START SAMPLE TERRAIN.bat.
echo.
if /i not "%~1"=="nopause" pause
endlocal
exit /b 0
