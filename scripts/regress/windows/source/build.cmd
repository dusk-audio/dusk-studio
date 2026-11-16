@echo off
rem usage: build.cmd <commit>
rem CI's targets at CI's -j4. "-k 0" only keeps ninja going past a failure, so
rem one pass collects every compile error; the verdict is the same.
rem A build that did work records <commit> in stamps\built-commit in this same
rem call, so no interruption can fall between the two. One that did no work
rem is first checked against the commit the tree last built (built-check.ps1).
setlocal
call "%~dp0env.cmd" toolchain || exit /b 1
cd /d "%ROOT%\src" || exit /b 1
set CONFIGURE=0
if not exist build-tests\CMakeCache.txt set CONFIGURE=1
fc /b "%~dp0configure.cmd" "%ROOT%\stamps\configure.cmd" >nul 2>&1 || set CONFIGURE=1
if "%CONFIGURE%"=="1" (
  call "%~dp0configure.cmd" || exit /b 1
)
set "LOG=%ROOT%\tmp\build.log"
cmake --build "%ROOT%\src\build-tests" --target DuskStudio dusk-studio-tests dusk-windows-software-opengl-probe -j4 -- -k 0 >"%LOG%" 2>&1
set "RC=%ERRORLEVEL%"
type "%LOG%"
if not "%RC%"=="0" exit /b %RC%
findstr /x /c:"ninja: no work to do." "%LOG%" >nul
if errorlevel 1 (
  >"%ROOT%\stamps\built-commit" echo %~1
  echo built-commit: built %~1
  exit /b 0
)
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%~dp0built-check.ps1" -Root "%ROOT%" -Head "%~1" -BuildLog "%LOG%"
exit /b %ERRORLEVEL%
