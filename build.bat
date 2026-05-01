@echo off
setlocal
where clang >nul 2>nul
if not errorlevel 1 (
    clang -std=c99 -O2 -Wall -Wextra -o perf-run.exe perf-run.c -luser32
    exit /b %errorlevel%
)
where cl >nul 2>nul
if not errorlevel 1 (
    cl /nologo /W4 /O2 /TC perf-run.c /link user32.lib
    exit /b %errorlevel%
)
echo Error: no C compiler found (looked for clang, cl).
exit /b 1
