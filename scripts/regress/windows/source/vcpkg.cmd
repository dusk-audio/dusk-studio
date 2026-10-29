@echo off
rem The manifest dependencies, as CI installs them, into src\vcpkg_installed.
rem Reads the account's default binary cache and writes only to the tree's own.
setlocal
call "%~dp0env.cmd" toolchain || exit /b 1
set VCPKG_DISABLE_METRICS=1
if not exist "%ROOT%\v\cache" mkdir "%ROOT%\v\cache"
set "VCPKG_BINARY_SOURCES=clear;files,%LOCALAPPDATA%\vcpkg\archives,read;files,%ROOT%\v\cache,readwrite"
cd /d "%ROOT%\src" || exit /b 1
"%DUSK_WIN_VCPKG%\vcpkg.exe" install "--vcpkg-root=%DUSK_WIN_VCPKG%" --triplet x64-windows-static --x-buildtrees-root=%ROOT%\v\bt --x-packages-root=%ROOT%\v\pk --downloads-root=%ROOT%\v\dl --clean-after-build || exit /b 1
copy /y "%ROOT%\src\vcpkg.json" "%ROOT%\stamps\vcpkg.json" >nul || exit /b 1
exit /b 0
