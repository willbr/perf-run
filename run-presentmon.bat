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

set "PRESENTMON=C:\Users\wjbr\scoop\apps\presentmon\current\PresentMon.exe"

if not exist perf-run.exe (
    echo perf-run.exe not found in %CD%. Run build.bat first.
    pause
    exit /b 1
)

if not exist "%PRESENTMON%" (
    echo PresentMon not found at "%PRESENTMON%".
    echo Edit run-presentmon.bat to point PRESENTMON at your install.
    pause
    exit /b 1
)

perf-run.exe --warmup 5 --n 200 --settle 50 --presentmon "%PRESENTMON%"

echo.
pause
