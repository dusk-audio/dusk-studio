@echo off
rem CI's targets at CI's -j4. "-k 0" only keeps ninja going past a failure, so
rem one pass collects every compile error; the verdict is the same.
setlocal
call "%~dp0env.cmd" toolchain || exit /b 1
cd /d "%ROOT%\src" || exit /b 1
set CONFIGURE=0
if not exist build-tests\CMakeCache.txt set CONFIGURE=1
fc /b "%~dp0configure.cmd" "%ROOT%\stamps\configure.cmd" >nul 2>&1 || set CONFIGURE=1
if "%CONFIGURE%"=="1" (
  call "%~dp0configure.cmd" || exit /b 1
)
cmake --build "%ROOT%\src\build-tests" --target DuskStudio dusk-studio-tests dusk-windows-software-opengl-probe -j4 -- -k 0
exit /b %ERRORLEVEL%
