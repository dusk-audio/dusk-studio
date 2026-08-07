# Building Dusk Studio on Windows

Dusk Studio targets Linux as its primary platform, but the codebase is JUCE 8 / C++17 with platform-specific code properly gated, so a Windows build is straightforward. JUCE's WASAPI / ASIO backends replace the Linux ALSA / PipeWire path automatically.

This document is aimed at a developer with a Windows machine who has been handed the source tree and wants to compile and run it.

## Prerequisites (one-time install)

1. **Visual Studio 2022 Community** (free from https://visualstudio.microsoft.com/).
   - During install, check the **"Desktop development with C++"** workload.
   - This brings MSVC, the Windows 10/11 SDK, and CMake. No separate CMake install needed.
2. **Git for Windows**: https://git-scm.com/download/win
3. **vcpkg**, for libsndfile and LAME. libsndfile is not optional on any platform — all audio file I/O goes through it and configure hard-fails without it ([CMakeLists.txt:594-607](CMakeLists.txt#L594-L607)). LAME is what enables MP3 bounce. Install the same static triplet CI uses, then hand CMake the prefix:

   ```cmd
   vcpkg install libsndfile:x64-windows-static mp3lame:x64-windows-static
   ```

   and add `-DCMAKE_PREFIX_PATH=<vcpkg-root>/installed/x64-windows-static` to the configure line, forward slashes as with the DPF paths below. `%VCPKG_INSTALLATION_ROOT%` holds that root on the CI images; on a hand-installed vcpkg, substitute wherever you cloned it.
4. **Steinberg ASIO SDK** — effectively required for the Visual Studio flow in this document, despite reading like an extra. A multi-config generator with Release among its configurations fails the configure without it ([CMakeLists.txt:693-700](CMakeLists.txt#L693-L700) and [:727-734](CMakeLists.txt#L727-L734)), which is exactly what `-G "Visual Studio 17 2022"` gives you: a shipping Windows binary must not silently come out WASAPI-only. Download from https://www.steinberg.net/asiosdk, accept the EULA, unzip somewhere stable, then pass `-DASIOSDK_PATH=C:/path/to/asiosdk` (its `common/` must contain `iasiodrv.h`). To skip the download on a first dev build, pass `-DDUSKSTUDIO_REQUIRE_ASIO=OFF` instead and accept WASAPI only.

## Repository layout

Dusk Studio expects four sibling repositories to be present alongside its own checkout:

```
C:\dev\
├── dusk-studio\       (this repo)
├── JUCE\              (JUCE 8.0.x, the framework)
├── plugins\           (Dusk Audio plugins, donor DSP)
├── DPF\               (DISTRHO Plugin Framework — native notepad UI)
└── DPF-Widgets\       (Dear ImGui layer for DPF)
```

CMake auto-discovers these. If you put them elsewhere, pass `-DJUCE_PATH=...`, `-DDUSK_PLUGINS_PATH=...`, `-DDPF_PATH=...`, and `-DDPF_WIDGETS_PATH=...` at configure time.

### Clone everything

Open a terminal (PowerShell, cmd, or Git Bash). All of these repos are public, no auth needed.

```cmd
cd C:\dev
git clone --recurse-submodules https://github.com/dusk-audio/dusk-studio.git
git clone --branch 8.0.4 https://github.com/juce-framework/JUCE.git
git clone https://github.com/dusk-audio/dusk-audio-plugins.git plugins
```

`--recurse-submodules` matters: `external/sfizz` carries the SF2 / multisample instrument engine, and CMake gates it purely on the header being present ([CMakeLists.txt:1163](CMakeLists.txt#L1163)) — clone without it and the feature is gone with no diagnostic. If you already cloned flat, run `git submodule update --init --recursive`.

The explicit `plugins` target on the third clone is mandatory: CMake auto-discovery looks for a sibling directory named `plugins\` and nothing else ([CMakeLists.txt:358-367](CMakeLists.txt#L358-L367)). The repo itself is named `dusk-audio-plugins` on GitHub, so without the explicit target you'd get a directory CMake can't find — and it warns rather than failing, leaving you with a recorder that has no EQ, compressor, or tape.

The Dusk Studio repo's own directory name (`dusk-studio\`) doesn't matter to the build, so rename it if you prefer.

### The native notepad (DPF + Dear ImGui)

The session notepad is a native window built on DPF/DGL plus the Dear ImGui layer from DPF-Widgets, rather than a JUCE component. Both come from Dusk-owned forks pinned to the revisions CI builds:

```cmd
cd C:\dev
git clone https://github.com/dusk-audio/DPF.git
git -C DPF checkout f9fbc62af6fa7ce638a6f1e1482896c385a4955e
git -C DPF submodule update --init
git clone https://github.com/dusk-audio/DPF-Widgets.git
git -C DPF-Widgets checkout 730da6397904da66d99667c1cb30fc77fc3d794a
```

Clone then check out the SHA, rather than cloning a branch: DPF's pin is the tip of `fix/wayland-review-findings`, which was never merged to that fork's `main`. (DPF-Widgets' pin happens to be its `main` tip today, but pin it the same way.) Neither branch may be deleted upstream — a plain clone would stop reaching the commit, and CI fetches the same SHAs. The submodule step is not optional either: DGL pulls its windowing layer from `dgl/src/pugl-upstream`. The pins live in [.github/actions/clone-dpf-stack/action.yml](.github/actions/clone-dpf-stack/action.yml), the single source of truth for every workflow.

Missing either checkout, `DUSKSTUDIO_ENABLE_NATIVE_NOTEPAD` defaults to **OFF** and configure says so once, quietly:

```
-- Native notepad: DPF / DPF-Widgets not found - disabled
```

The build otherwise completes as normal, but opening the notepad reports *"Notepad unavailable: built without the native notepad UI"*. Passing `-DDUSKSTUDIO_ENABLE_NATIVE_NOTEPAD=ON` with a checkout missing makes it a configure error instead.

That OFF is sticky, because `DUSKSTUDIO_ENABLE_NATIVE_NOTEPAD` is a **cached** CMake option — the one dependency here a later reconfigure won't pick up on its own. Configure `build\` before cloning DPF and cloning it afterwards changes nothing on the next configure, and the "not found" line stops printing too. Either configure into a fresh build directory or add `-DDUSKSTUDIO_ENABLE_NATIVE_NOTEPAD=ON` to the configure command below.

If you override the paths explicitly, give DPF **forward slashes** — `-DDPF_PATH=C:/dev/DPF`. DPF's own CMake re-parses the value, and backslashes come through as invalid escapes.

## Configure + build

From the Dusk Studio directory:

```cmd
cd C:\dev\dusk-studio
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_PREFIX_PATH=%VCPKG_INSTALLATION_ROOT%/installed/x64-windows-static ^
  -DASIOSDK_PATH=C:/dev/asiosdk
cmake --build build --config Release -j
```

Swap `-DASIOSDK_PATH=...` for `-DDUSKSTUDIO_REQUIRE_ASIO=OFF` if you skipped the SDK download. The first configure pulls in JUCE's CMake helpers and may take a minute. Subsequent configures are fast.

The built binary lands at:

```
C:\dev\dusk-studio\build\DuskStudio_artefacts\Release\DuskStudio.exe
```

Double-click to run, or launch from the terminal.

### Building Debug instead

```cmd
cmake --build build --config Debug -j
```

Debug binary appears under `build\DuskStudio_artefacts\Debug\`.

### Opening in Visual Studio

```cmd
start build\DuskStudio.sln
```

In VS, right-click the **DuskStudio** project → **Set as Startup Project** → F5 to debug.

## Overriding paths (if not using the sibling layout)

```cmd
cmake -S . -B build ^
  -DJUCE_PATH=C:/some/other/JUCE ^
  -DDUSK_PLUGINS_PATH=C:/some/other/plugins ^
  -DDPF_PATH=C:/some/other/DPF ^
  -DDPF_WIDGETS_PATH=C:/some/other/DPF-Widgets ^
  -G "Visual Studio 17 2022" -A x64
```

## Tests (optional)

Dusk Studio has Catch2 unit tests behind a CMake flag:

```cmd
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests --config Release -j
ctest --test-dir build-tests --output-on-failure -C Release
```

Use a separate `build-tests\` directory so the two configurations don't fight over CMake cache state.

## Headless self-test (optional)

Set an environment variable to run Dusk Studio's internal DSP self-test on startup instead of opening the GUI:

```cmd
set DUSKSTUDIO_RUN_SELFTEST=1
build\DuskStudio_artefacts\Release\DuskStudio.exe
```

Useful for confirming the audio engine wires up correctly without needing to drive the UI.

## Known caveats on Windows

- **PlatformWindowing_Windows.cpp is a stub.** Most things work; the file exists as the place to land Windows-specific window-management fixes if/when XEmbed-equivalent bugs surface. CMake picks the per-platform implementation at [CMakeLists.txt:640-646](CMakeLists.txt#L640-L646).
- **No ASIO without the SDK.** WASAPI is the default; ASIO requires the SDK download above. Most users will be fine on WASAPI.
- **The ALSA backend is not compiled.** [CMakeLists.txt:753](CMakeLists.txt#L753) gates Dusk Studio's custom ALSA `AudioIODeviceType` behind `UNIX AND NOT APPLE`. Windows falls through to JUCE's stock WASAPI/ASIO types.
- **Compiler warnings.** Project is primarily developed on Clang/GCC. MSVC may emit warnings; none are fatal. `/WX` (warnings-as-errors) is not enabled.
- **MinGW/MSYS2 not tested.** Stick to MSVC via Visual Studio 2022.

## Reporting build issues

If the build fails, capture:

1. The full CMake configure output (`cmake -S . -B build ...`)
2. The full build output (`cmake --build build ...`)
3. `cmake --version` and the VS version used.

Send those to Marc and we can debug from there.
