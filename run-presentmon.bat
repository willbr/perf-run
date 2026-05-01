@echo off
REM Run perf-run with PresentMon enabled. Self-elevates via UAC if needed
REM (ETW sessions require admin or "Performance Log Users" membership).

net session >nul 2>&1
if errorlevel 1 (
    echo Requesting elevation...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

cd /d "%~dp0"

if not exist perf-run.exe (
    echo perf-run.exe not found in %CD%. Run build.bat first.
    pause
    exit /b 1
)

where presentmon >nul 2>&1
if errorlevel 1 (
    echo PresentMon not found on PATH.
    echo Install it with: scoop install presentmon
    echo or download from https://github.com/GameTechDev/PresentMon
    pause
    exit /b 1
)

perf-run.exe --warmup 5 --n 200 --settle 50 --presentmon

echo.
pause
