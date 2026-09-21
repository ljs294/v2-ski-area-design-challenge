@echo off
setlocal
set "PROJECT=%~dp0SkiAreaDesignChallenge.uproject"
set "EDITOR=C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
if not exist "%EDITOR%" (
  echo Unreal Editor 5.8 was not found at:
  echo %EDITOR%
  exit /b 1
)
echo Starting the local editor-only Unreal MCP server on 127.0.0.1:8000...
start "Mountain Planner - Unreal MCP" "%EDITOR%" "%PROJECT%" -ModelContextProtocolStartServer -ModelContextProtocolPort=8000
endlocal
