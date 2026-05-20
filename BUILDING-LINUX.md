# Building Focal on Linux

Focal targets Linux as its primary platform. ALSA is the default audio backend; PipeWire's JACK shim works too. JUCE 8 / C++17, no exotic toolchains.

This document is aimed at a developer with a Linux machine who has been handed the source tree and wants to compile and run it. Patreon supporters who just want a precompiled AppImage should grab one from the Patreon post instead.

## Prerequisites (one-time install)

Verified on Ubuntu 22.04 LTS and Fedora 39. Other distros work with equivalent package names.

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config git \
  libasound2-dev libjack-jackd2-dev \
  ladspa-sdk \
  libcurl4-openssl-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libxss-dev \
  libwebkit2gtk-4.0-dev libglu1-mesa-dev \
  libwayland-dev libxkbcommon-dev libdecor-0-dev
```

### Fedora

```bash
sudo dnf install -y \
  gcc-c++ cmake ninja-build pkgconf-pkg-config git \
  alsa-lib-devel jack-audio-connection-kit-devel \
  ladspa-devel \
  libcurl-devel freetype-devel fontconfig-devel \
  libX11-devel libXcomposite-devel libXcursor-devel libXext-devel \
  libXinerama-devel libXrandr-devel libXrender-devel libXScrnSaver-devel \
  webkit2gtk4.0-devel mesa-libGLU-devel \
  wayland-devel libxkbcommon-devel libdecor-devel
```

## Repository layout

Focal expects two sibling repositories alongside its own checkout:

```
~/projects/
├── Focal/             (this repo)
├── JUCE-wayland/      (plugdata-team fork, branch: wayland-juce8)
└── plugins-main/      (Dusk Audio plugins, donor DSP — main branch worktree)
```

CMake auto-discovers these. Override with `-DJUCE_PATH=...` / `-DDUSK_PLUGINS_PATH=...` if you keep them elsewhere.

### Why the JUCE-wayland fork (Linux-only)

Stock JUCE on Linux uses X11 for top-level windows, which under GNOME / Wayland sessions runs through XWayland. Closing certain plugin editors (Diva, AM_VST3 family) crashes mutter via the `meta_window_unmanage` assertion, taking the whole desktop session down. The [plugdata-team/JUCE wayland-juce8](https://github.com/plugdata-team/JUCE) fork uses libwayland-client + libdecor for top-level windows directly, bypassing XWayland for the main surface.

Cross-platform Focal source compiles against either upstream JUCE or the fork; the wayland fork is required at runtime on Linux desktops. Mac dev uses upstream JUCE.

### Clone everything

```bash
cd ~/projects
git clone https://github.com/dusk-audio/Focal.git
git clone --branch wayland-juce8 https://github.com/plugdata-team/JUCE.git JUCE-wayland
git clone https://github.com/dusk-audio/dusk-audio-plugins.git plugins-main
```

If you also have an upstream `JUCE/` and a feature-branch `plugins/` sibling for cross-OS dev, CMake prefers `JUCE-wayland/` and `plugins-main/` first per [CLAUDE.md cross-OS table](CLAUDE.md).

## Configure + build

From the Focal directory:

```bash
cd ~/projects/Focal
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j$(nproc)
```

First configure pulls in JUCE's CMake helpers and may take a minute. Subsequent configures are fast.

The built binary lands at:

```
build-linux/Focal_artefacts/Release/Focal
```

Run it from the terminal:

```bash
./build-linux/Focal_artefacts/Release/Focal
```

### Building Debug instead

```bash
cmake -S . -B build-linux-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux-debug -j$(nproc)
```

### Overriding paths (if not using the sibling layout)

```bash
cmake -S . -B build-linux \
  -DJUCE_PATH=/some/other/JUCE \
  -DDUSK_PLUGINS_PATH=/some/other/plugins
```

## Tests

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DFOCAL_BUILD_TESTS=ON
cmake --build build-tests --target focal-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

Use a separate `build-tests/` directory so the two configurations don't fight over CMake cache state.

### Under AddressSanitizer + UBSan

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFOCAL_BUILD_TESTS=ON -DFOCAL_ENABLE_ASAN=ON
cmake --build build-asan --target focal-tests -j$(nproc)
ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0" \
  ctest --test-dir build-asan --output-on-failure
```

CI runs this nightly via [.github/workflows/linux-sanitizer.yml](.github/workflows/linux-sanitizer.yml).

## Headless self-test

Drives the synthetic DSP pipeline without opening the GUI:

```bash
FOCAL_RUN_SELFTEST=1 ./build-linux/Focal_artefacts/Release/Focal
```

Useful for confirming the audio engine wires up correctly without needing to drive the UI.

## Audio backend selection

Focal ships its own ALSA backend ([src/engine/alsa/](src/engine/alsa/)) plus the stock JUCE backends (JACK / ALSA-via-JUCE). Pick from the **Audio Device** panel inside Focal.

- **ALSA (Focal native)** — direct hardware access, lowest latency, no graph hops. Default.
- **JACK** — works against PipeWire's JACK shim or a real JACK server. Use if your interface is owned by PipeWire's graph and you want to route through there.

The Focal-native ALSA backend handles USB hot-unplug by surfacing the device error to the engine, which finalises any in-flight take. Details in [docs/USER_GUIDE.md](docs/USER_GUIDE.md#troubleshooting).

## Out-of-process plugin host (Linux only, currently)

```bash
FOCAL_USE_OOP_PLUGINS=1 ./build-linux/Focal_artefacts/Release/Focal
```

Routes new plugin loads through the `focal-plugin-host` child process so a misbehaving plugin can't take down the host. Currently Linux-only via `memfd_create` + `futex`. macOS (Mach ports) and Windows (named pipes) ports land in 1.0.

## Packaging an AppImage

See [packaging/README.md](packaging/README.md). Requires `linuxdeploy` and a 256×256 PNG icon at `packaging/focal.png` (not committed — provide your own).

## Known caveats on Linux

- **JUCE-wayland fork is required at runtime.** The fork has five local commits (XEmbed mapping, X11-on-Wayland fix, peer-creation latch, XEmbed bg fix) on top of plugdata-team's `wayland-juce8` branch. Vanilla upstream JUCE will compile (the `addDefaultFormats` shim in [src/engine/JuceCompat.h](src/engine/JuceCompat.h) abstracts the API split) but will hit the mutter crash on plugin-editor close under GNOME/Wayland. See [CLAUDE.md](CLAUDE.md) for context.
- **Plugin destructors are intentionally leaked at shutdown.** [src/FocalApp.cpp](src/FocalApp.cpp) `leakAllPluginInstancesForShutdown` is a Linux-only workaround for Diva's `__cxa_pure_virtual` abort in `~AM_VST3_ViewInterface`. The OS reclaims memory on process exit.
- **PipeWire native backend is not implemented.** Focal uses ALSA directly or via JUCE's JACK backend (which speaks to PipeWire's JACK shim). A native PipeWire-graph integration would be a future addition, not blocking.
- **Compiler warnings.** The vendored Dusk DSP `.cpp` files compiled into Focal emit shadow/sign-conversion warnings. `FOCAL_STRICT_WARNINGS=ON` (`-Werror`) is opt-in but not yet enabled in CI until those are cleaned upstream or wrapped with per-source overrides.

## Reporting build issues

If the build fails, capture:

1. Full CMake configure output (`cmake -S . -B build-linux ...`)
2. Full build output (`cmake --build build-linux -j ...`)
3. `cmake --version`, `gcc --version` or `clang --version`, distro + kernel (`uname -a`, `cat /etc/os-release`)
4. Output of `./build-linux/Focal_artefacts/Release/Focal --version` if you got that far

Open an issue on GitHub or paste into the Patreon support thread.
