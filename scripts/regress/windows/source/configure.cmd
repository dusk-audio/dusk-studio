@echo off
rem The configure step of .github/workflows/windows-tests.yml; change the two
rem together. Left out: the sccache launchers. Added: Python3_EXECUTABLE,
rem since the guest has no system Python and CI requires the Python-backed
rem ctest entries. build.cmd runs this whenever it differs from the copy that
rem last configured the tree.
setlocal
call "%~dp0env.cmd" toolchain || exit /b 1
cd /d "%ROOT%\src" || exit /b 1
rem DAF's cmake re-parses these paths, so it gets forward slashes as in CI.
set "R=%ROOT:\=/%"
set "JUCE=%DUSK_WIN_JUCE:\=/%"
set "ASIO=%DUSK_WIN_ASIOSDK:\=/%"
cmake -S . -B build-tests ^
  -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=cl ^
  -DCMAKE_CXX_COMPILER=cl ^
  -DDUSKSTUDIO_CCACHE:FILEPATH=OFF ^
  -DDUSKSTUDIO_BUILD_TESTS=ON ^
  "-DJUCE_PATH=%JUCE%" ^
  "-DDUSK_PLUGINS_PATH=%R%/deps/plugins" ^
  "-DDAF_PATH=%R%/deps/DAF" ^
  "-DASIOSDK_PATH=%ASIO%" ^
  "-DCMAKE_PREFIX_PATH=%R%/src/vcpkg_installed/x64-windows-static" ^
  "-DDUSKSTUDIO_WINDOWS_SOFTWARE_OPENGL_DIR=%R%/deps/mesa" ^
  -DDUSKSTUDIO_ENABLE_NATIVE_UI=ON ^
  -DDUSKSTUDIO_NATIVE_CLAP=ON ^
  -DDUSKSTUDIO_REQUIRE_LAME=ON ^
  -DDUSKSTUDIO_NATIVE_VST3=ON ^
  "-DPython3_EXECUTABLE=%R%/deps/python/tools/python.exe" || exit /b 1
for %%o in (DUSKSTUDIO_ENABLE_NATIVE_UI DUSKSTUDIO_NATIVE_CLAP DUSKSTUDIO_NATIVE_VST3) do (
  findstr /b /c:"%%o:BOOL=ON" build-tests\CMakeCache.txt >nul || (echo error: %%o was not enabled in the test configure & exit /b 1)
)
copy /y "%~f0" "%ROOT%\stamps\configure.cmd" >nul || exit /b 1
exit /b 0
