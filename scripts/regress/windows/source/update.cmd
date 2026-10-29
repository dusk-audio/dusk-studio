@echo off
rem usage: update.cmd <commit> <base>[+<base>...] [<bundle>]
rem Brings the tree to <commit>: each base fetched from GitHub by hash, then
rem the bundle of the local commits on top of them when there are any.
rem Submodules follow, then the donor at DONOR_REV, DAF at the tip of main as
rem CI clones it, and Mesa, Python and the vcpkg packages whenever the file
rem that pins them changed since they were last installed.
setlocal
call "%~dp0env.cmd" || exit /b 1
call "%~dp0setup.cmd" || exit /b 1
set "COMMIT=%~1"
set "BASE=%~2"
set "BUNDLE=%~3"
if "%BASE%"=="" (echo usage: update.cmd ^<commit^> ^<base^>[+^<base^>...] [^<bundle^>] & exit /b 2)
cd /d "%ROOT%\src" || exit /b 1

git fetch -q --depth 1 origin %BASE:+= % || exit /b 1
if not "%BUNDLE%"=="" (
  git bundle verify -q "%BUNDLE%" || exit /b 1
  git fetch -q "%BUNDLE%" "+HEAD:refs/regress/head" || exit /b 1
)
git -c advice.detachedHead=false checkout -q --detach %COMMIT% || exit /b 1
for /f %%h in ('git rev-parse HEAD') do set "AT=%%h"
if /i not "%AT%"=="%COMMIT%" (echo error: src is at %AT%, not %COMMIT% & exit /b 1)
git -c protocol.version=2 submodule update --init --force --depth=1 --recursive || exit /b 1
echo HEAD %AT%

set /p DONOR_REV=<DONOR_REV
set "DONOR_AT="
for /f %%h in ('git -C "%ROOT%\deps\plugins" rev-parse HEAD 2^>nul') do set "DONOR_AT=%%h"
if /i not "%DONOR_AT%"=="%DONOR_REV%" (
  git -C "%ROOT%\deps\plugins" fetch -q --depth 1 origin %DONOR_REV% || exit /b 1
  git -C "%ROOT%\deps\plugins" checkout -q FETCH_HEAD || exit /b 1
)
for /f %%h in ('git -C "%ROOT%\deps\plugins" rev-parse HEAD') do set "DONOR_AT=%%h"
if /i not "%DONOR_AT%"=="%DONOR_REV%" (echo error: donor is at %DONOR_AT%, DONOR_REV is %DONOR_REV% & exit /b 1)
echo donor %DONOR_AT%

git -C "%ROOT%\deps\DAF" fetch -q --depth 1 origin main || exit /b 1
git -C "%ROOT%\deps\DAF" checkout -q FETCH_HEAD || exit /b 1
for /f %%h in ('git -C "%ROOT%\deps\DAF" rev-parse HEAD') do echo DAF %%h

fc /b "%ROOT%\src\scripts\fetch-windows-software-opengl.ps1" "%ROOT%\stamps\fetch-windows-software-opengl.ps1" >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deps.ps1" || exit /b 1
)
fc /b "%ROOT%\src\vcpkg.json" "%ROOT%\stamps\vcpkg.json" >nul 2>&1
if errorlevel 1 (
  call "%~dp0vcpkg.cmd" || exit /b 1
)

git status --short
exit /b 0
