@echo off
rem Creates whatever part of the tree is missing: the shallow clone and the
rem empty donor and DAF repositories update.cmd fetches into. A no-op on a
rem tree that has them.
setlocal
call "%~dp0env.cmd" || exit /b 1
cd /d "%ROOT%" || exit /b 1
if not exist "%ROOT%\src\.git" (
  echo cloning into %ROOT%\src
  git -c core.longpaths=true clone --depth 1 https://github.com/dusk-audio/dusk-studio.git src || exit /b 1
)
git -C "%ROOT%\src" config core.longpaths true || exit /b 1
if not exist "%ROOT%\deps" mkdir "%ROOT%\deps"
call :repo plugins https://github.com/dusk-audio/dusk-audio-plugins.git || exit /b 1
call :repo DAF https://github.com/dusk-audio/DAF.git || exit /b 1
exit /b 0

:repo
if exist "%ROOT%\deps\%1\.git" exit /b 0
git init -q "%ROOT%\deps\%1" || exit /b 1
git -C "%ROOT%\deps\%1" remote add origin %2 || exit /b 1
git -C "%ROOT%\deps\%1" config core.longpaths true || exit /b 1
exit /b 0
