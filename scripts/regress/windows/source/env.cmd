@echo off
rem Called first by every script in this folder. The tree root is the parent
rem of the folder the runner copies these scripts into; set DUSK_WIN_ROOT to
rem point them at another tree. "call env.cmd toolchain" also sets up CI's
rem msvc-dev-cmd (amd64) equivalent and the private Python.
if not defined DUSK_WIN_ROOT for %%r in ("%~dp0..") do set "DUSK_WIN_ROOT=%%~fr"
set "ROOT=%DUSK_WIN_ROOT%"
if not defined DUSK_WIN_VCVARS set "DUSK_WIN_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined DUSK_WIN_JUCE set "DUSK_WIN_JUCE=C:\Users\marc\JUCE-8.0.4"
if not defined DUSK_WIN_ASIOSDK set "DUSK_WIN_ASIOSDK=C:\Users\marc\SDKs\asiosdk"
if not defined DUSK_WIN_VCPKG set "DUSK_WIN_VCPKG=C:\Users\marc\vcpkg"
if not defined DUSK_WIN_7ZIP set "DUSK_WIN_7ZIP=C:\Program Files\7-Zip"
set "TEMP=%ROOT%\tmp"
set "TMP=%ROOT%\tmp"
for %%d in (logs tmp stamps) do if not exist "%ROOT%\%%d" mkdir "%ROOT%\%%d"
if /i not "%~1"=="toolchain" exit /b 0
call "%DUSK_WIN_VCVARS%" >nul || exit /b 1
set "PATH=%ROOT%\deps\python\tools;%PATH%"
exit /b 0
