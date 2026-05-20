# Building Focal on Windows

Focal targets Linux as its primary platform, but the codebase is JUCE 8 / C++17 with platform-specific code properly gated, so a Windows build is straightforward. JUCE's WASAPI / ASIO backends replace the Linux ALSA / PipeWire path automatically.

This document is aimed at a developer with a Windows machine who has been handed the source tree and wants to compile and run it.

## Prerequisites (one-time install)

1. **Visual Studio 2022 Community** (free from https://visualstudio.microsoft.com/).
   - During install, check the **"Desktop development with C++"** workload.
   - This brings MSVC, the Windows 10/11 SDK, and CMake. No separate CMake install needed.
2. **Git for Windows**: https://git-scm.com/download/win
3. *(optional)* **Steinberg ASIO SDK**, only if you want ASIO driver support in the audio device picker. Download from https://www.steinberg.net/asiosdk, accept the EULA, unzip somewhere stable, then pass `-DJUCE_ASIO_SDK_PATH=C:/path/to/asiosdk` at CMake configure time. Skip this for a first build; WASAPI works fine.

## Repository layout

Focal expects two sibling repositories to be present alongside its own checkout:

```
C:\dev\
├── dusk-studio-daw\       (this repo)
├── JUCE\            (JUCE 8.0.x, the framework)
└── plugins\         (Dusk Audio plugins, donor DSP)
```

CMake auto-discovers these. If you put them elsewhere, pass `-DJUCE_PATH=...` and `-DDUSK_PLUGINS_PATH=...` at configure time.

### Clone everything

Open a terminal (PowerShell, cmd, or Git Bash). Both Focal repos are public, no auth needed.

```cmd
cd C:\dev
git clone https://github.com/dusk-audio/dusk-studio-daw.git
git clone --branch 8.0.4 https://github.com/juce-framework/JUCE.git
git clone https://github.com/dusk-audio/dusk-audio-plugins.git plugins
```

The explicit `plugins` target on the third clone is mandatory: CMake auto-discovery looks for a sibling directory named `plugins\` or `plugins-main\` ([CMakeLists.txt:160-168](CMakeLists.txt#L160-L168)). The repo itself is named `dusk-audio-plugins` on GitHub, so without the explicit target you'd get a directory CMake can't find.

The Focal repo's own directory name (`dusk-studio-daw\`) doesn't matter to the build, but feel free to `git clone <url> Focal` if you prefer.

## Configure + build

From the Focal directory:

```cmd
cd C:\dev\dusk-studio-daw
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release -j
```

The first configure pulls in JUCE's CMake helpers and may take a minute. Subsequent configures are fast.

The built binary lands at:

```
C:\dev\dusk-studio-daw\build\DuskStudio_artefacts\Release\Focal.exe
```

Double-click to run, or launch from the terminal.

### Building Debug instead

```cmd
cmake --build build --config Debug -j
```

Debug binary appears under `build\DuskStudio_artefacts\Debug\`.

### Opening in Visual Studio

```cmd
start build\Focal.sln
```

In VS, right-click the **Focal** project → **Set as Startup Project** → F5 to debug.

## Overriding paths (if not using the sibling layout)

```cmd
cmake -S . -B build ^
  -DJUCE_PATH=C:/some/other/JUCE ^
  -DDUSK_PLUGINS_PATH=C:/some/other/plugins ^
  -G "Visual Studio 17 2022" -A x64
```

## Tests (optional)

Focal has Catch2 unit tests behind a CMake flag:

```cmd
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DFOCAL_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests --config Release -j
ctest --test-dir build-tests --output-on-failure -C Release
```

Use a separate `build-tests\` directory so the two configurations don't fight over CMake cache state.

## Headless self-test (optional)

Set an environment variable to run Focal's internal DSP self-test on startup instead of opening the GUI:

```cmd
set DUSKSTUDIO_RUN_SELFTEST=1
build\DuskStudio_artefacts\Release\Focal.exe
```

Useful for confirming the audio engine wires up correctly without needing to drive the UI.

## Known caveats on Windows

- **PlatformWindowing_Windows.cpp is a stub.** Most things work; the file exists as the place to land Windows-specific window-management fixes if/when XEmbed-equivalent bugs surface. The Linux-only `JUCE_XEmbedComponent` source isn't compiled on Windows ([CMakeLists.txt:57](CMakeLists.txt#L57)).
- **No ASIO without the SDK.** WASAPI is the default; ASIO requires the SDK download above. Most users will be fine on WASAPI.
- **The ALSA backend is not compiled.** [CMakeLists.txt:263](CMakeLists.txt#L263) gates Focal's custom ALSA `AudioIODeviceType` behind `UNIX AND NOT APPLE`. Windows falls through to JUCE's stock WASAPI/ASIO types.
- **Compiler warnings.** Project is primarily developed on Clang/GCC. MSVC may emit warnings; none are fatal. `/WX` (warnings-as-errors) is not enabled.
- **MinGW/MSYS2 not tested.** Stick to MSVC via Visual Studio 2022.

## Reporting build issues

If the build fails, capture:

1. The full CMake configure output (`cmake -S . -B build ...`)
2. The full build output (`cmake --build build ...`)
3. `cmake --version` and the VS version used.

Send those to Marc and we can debug from there.
