@echo off
rem usage: ctest.cmd [list]
rem CI's "Run tests" step: "list" prints the inventory the runner checks CI's
rem required test names against, otherwise the whole suite runs serially.
setlocal
call "%~dp0env.cmd" toolchain || exit /b 1
cd /d "%ROOT%\src" || exit /b 1
if /i "%~1"=="list" (
  ctest --test-dir "%ROOT%\src\build-tests" -N
) else (
  ctest --test-dir "%ROOT%\src\build-tests" --output-on-failure
)
exit /b %ERRORLEVEL%
