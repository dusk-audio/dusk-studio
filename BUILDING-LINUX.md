# Building Dusk Studio on Linux

Dusk Studio targets Linux as its primary platform. It talks to the audio hardware through two backends of its own: a native PipeWire backend, preferred by default, and a native ALSA backend behind it. JUCE 8 / C++17, no exotic toolchains.

This document is aimed at a developer with a Linux machine who has been handed the source tree and wants to compile and run it. Patreon supporters who just want a precompiled binary should grab one from the Patreon post instead.

## Prerequisites (one-time install)

Verified on Ubuntu 22.04 LTS and Fedora 39. Other distros work with equivalent package names.

### Ubuntu / Debian

```bash
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config git \
  libasound2-dev libjack-jackd2-dev libpipewire-0.3-dev \
  libsndfile1-dev libmp3lame-dev \
  liblilv-dev libsuil-dev lv2-dev \
  ladspa-sdk \
  libcurl4-openssl-dev libfreetype-dev libfontconfig1-dev \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev libxss-dev \
  libwebkit2gtk-4.0-dev libgl1-mesa-dev libglu1-mesa-dev \
  libwayland-dev libxkbcommon-dev libdecor-0-dev
```

### Fedora

```bash
sudo dnf install -y \
  gcc-c++ cmake ninja-build pkgconf-pkg-config git \
  alsa-lib-devel jack-audio-connection-kit-devel pipewire-devel \
  libsndfile-devel lame-devel \
  lilv-devel suil-devel lv2-devel \
  ladspa-devel \
  libcurl-devel freetype-devel fontconfig-devel \
  libX11-devel libXcomposite-devel libXcursor-devel libXext-devel \
  libXinerama-devel libXrandr-devel libXrender-devel libXScrnSaver-devel \
  webkit2gtk4.0-devel mesa-libGL-devel mesa-libGLU-devel \
  wayland-devel libxkbcommon-devel libdecor-devel
```

libsndfile is not optional — all audio file I/O goes through it and configure fails without it. The PipeWire, LV2-host and MP3-bounce packages are probed at configure time and drop their feature silently when absent, and the probe result is cached, so install them before the first configure rather than after.

## Repository layout

Dusk Studio expects four sibling repositories alongside its own checkout:

```
~/projects/
├── dusk-studio/       (this repo)
├── JUCE-wayland/      (plugdata-team fork, branch: wayland-juce8)
├── plugins/           (Dusk Audio plugins, donor DSP)
├── DPF/               (DISTRHO Plugin Framework — native notepad UI)
└── DPF-Widgets/       (Dear ImGui layer for DPF)
```

CMake auto-discovers these. Override with `-DJUCE_PATH=...`, `-DDUSK_PLUGINS_PATH=...`, `-DDPF_PATH=...`, `-DDPF_WIDGETS_PATH=...` if you keep them elsewhere.

### Why the JUCE-wayland fork (Linux-only)

Stock JUCE on Linux uses X11 for top-level windows, which under GNOME / Wayland sessions runs through XWayland. Closing certain plugin editors (Diva, AM_VST3 family) crashes mutter via the `meta_window_unmanage` assertion, taking the whole desktop session down. The [plugdata-team/JUCE wayland-juce8](https://github.com/plugdata-team/JUCE) fork uses libwayland-client + libdecor for top-level windows directly, bypassing XWayland for the main surface.

Cross-platform Dusk Studio source compiles against either upstream JUCE or the fork; the wayland fork is required at runtime on Linux desktops. Mac dev uses upstream JUCE.

### Clone everything

```bash
cd ~/projects
git clone https://github.com/dusk-audio/dusk-studio.git
git clone --branch wayland-juce8 https://github.com/plugdata-team/JUCE.git JUCE-wayland
git clone https://github.com/dusk-audio/dusk-audio-plugins.git plugins
```

The explicit `plugins` target on the third clone is mandatory — the repo is named `dusk-audio-plugins` on GitHub, and `../plugins` is the only sibling directory CMake checks ([CMakeLists.txt:358-367](CMakeLists.txt#L358-L367)). Get it wrong and configure prints a warning rather than failing; the build then produces a recorder with no EQ, compressor, or tape.

If you also keep an upstream `JUCE/` sibling for cross-OS dev, CMake prefers `JUCE-wayland/` on Linux and falls back to `JUCE/` only if the fork isn't there.

### The native notepad (DPF + Dear ImGui)

The session notepad is Dusk Studio's first native UI window: DPF/DGL for the OpenGL surface, DPF-Widgets for the Dear ImGui layer. Both come from Dusk-owned forks so an upstream rebase can't break a build.

```bash
cd ~/projects
git clone https://github.com/dusk-audio/DPF.git
git -C DPF checkout f9fbc62af6fa7ce638a6f1e1482896c385a4955e
git -C DPF submodule update --init
git clone https://github.com/dusk-audio/DPF-Widgets.git
git -C DPF-Widgets checkout 730da6397904da66d99667c1cb30fc77fc3d794a
```

Clone then check out the SHA, rather than cloning a branch: DPF's pin is the tip of `fix/wayland-review-findings`, which was never merged to that fork's `main`. (DPF-Widgets' pin happens to be its `main` tip today, but pin it the same way.) Neither branch may be deleted upstream — a plain clone would stop reaching the commit, and CI fetches the same SHAs. The submodule step is not optional either: DGL pulls its windowing layer from `dgl/src/pugl-upstream`.

The pins live in [.github/actions/clone-dpf-stack/action.yml](.github/actions/clone-dpf-stack/action.yml), which is the single source of truth for every workflow — read them from there if it ever disagrees with the commands above.

Missing either checkout, `DUSKSTUDIO_ENABLE_NATIVE_NOTEPAD` defaults to **OFF** and configure prints one easily-missed line:

```
-- Native notepad: DPF / DPF-Widgets not found - disabled
```

The rest of the app builds and runs normally, but opening the notepad reports *"Notepad unavailable: built without the native notepad UI"*. Passing `-DDUSKSTUDIO_ENABLE_NATIVE_NOTEPAD=ON` with a checkout missing turns that into a configure error instead of a silent downgrade.

That OFF is sticky, because `DUSKSTUDIO_ENABLE_NATIVE_NOTEPAD` is a **cached** CMake option. Configure `build-linux/` before cloning DPF and cloning it afterwards changes nothing on the next `cmake -S . -B build-linux` — and the "not found" line stops printing too, so nothing hints that the notepad is still off. Either configure into a fresh build directory or force the cache:

```bash
cmake -S . -B build-linux -DDUSKSTUDIO_ENABLE_NATIVE_NOTEPAD=ON
```

## Configure + build

From the Dusk Studio directory:

```bash
cd ~/projects/dusk-studio
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j$(nproc)
```

First configure pulls in JUCE's CMake helpers and may take a minute. Subsequent configures are fast.

The built binary lands at:

```
build-linux/DuskStudio_artefacts/Release/DuskStudio
```

Run it from the terminal:

```bash
./build-linux/DuskStudio_artefacts/Release/DuskStudio
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
  -DDUSK_PLUGINS_PATH=/some/other/plugins \
  -DDPF_PATH=/some/other/DPF \
  -DDPF_WIDGETS_PATH=/some/other/DPF-Widgets
```

## Tests

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j$(nproc)
ctest --test-dir build-tests --output-on-failure
```

Use a separate `build-tests/` directory so the two configurations don't fight over CMake cache state.

### Under AddressSanitizer + UBSan

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDUSKSTUDIO_BUILD_TESTS=ON -DDUSKSTUDIO_ENABLE_ASAN=ON
cmake --build build-asan --target dusk-studio-tests -j$(nproc)
ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0" \
  ctest --test-dir build-asan --output-on-failure
```

CI runs this nightly via [.github/workflows/linux-sanitizer.yml](.github/workflows/linux-sanitizer.yml).

## Headless self-test

Drives the synthetic DSP pipeline without opening the GUI:

```bash
DUSKSTUDIO_RUN_SELFTEST=1 ./build-linux/DuskStudio_artefacts/Release/DuskStudio
```

Useful for confirming the audio engine wires up correctly without needing to drive the UI.

## Audio backend selection

Dusk Studio ships two backends of its own, both speaking to the system directly rather than through JUCE. Pick from the **Audio Device** panel inside Dusk Studio, where they appear as **PipeWire** and **ALSA**.

- **PipeWire** ([src/engine/pipewire/](src/engine/pipewire/)) — a single `pw_filter` node on the graph; every Sink / Source node is listed as its own device. Correct graph latency and a client that shows up as "Dusk Studio" instead of a generic JACK name. Preferred by default: it is registered first, and the panel opens on the first backend that enumerates any device ([src/engine/device/DeviceManager.cpp:204-218](src/engine/device/DeviceManager.cpp#L204-L218)).
- **ALSA** ([src/engine/alsa/](src/engine/alsa/)) — direct hardware access, no graph hops. What you get when PipeWire isn't running, and what to pick when you want the interface to yourself.

There is no JACK backend. Dusk Studio used to reach PipeWire through JUCE's JACK path over the pipewire-jack shim; the native backend replaced it, and on Linux the JUCE device layer isn't compiled at all ([CMakeLists.txt:596-607](CMakeLists.txt#L596-L607)). To feed other applications, patch Dusk Studio inside PipeWire's graph with qpwgraph or Helvum.

The Dusk Studio-native ALSA backend handles USB hot-unplug by surfacing the device error to the engine, which finalises any in-flight take. Details in [MANUAL.md](MANUAL.md#audio-device-disconnected-mid-session).

## Out-of-process plugin host (opt-in, all platforms)

```bash
DUSKSTUDIO_USE_OOP_PLUGINS=1 ./build-linux/DuskStudio_artefacts/Release/DuskStudio
```

Routes new plugin loads through the `dusk-studio-plugin-host` child process so a misbehaving plugin can't take down the host. Implemented on all three OSes (Linux via `memfd_create` + `futex`, macOS via Mach ports, Windows via named pipes). In-process is the default for the lowest editor latency; set `DUSKSTUDIO_USE_OOP_PLUGINS=1` to opt into the crash-isolating sandbox. Plugin **scanning** is always sandboxed regardless of this flag.

## Packaging the Linux tarball

See [packaging/README.md](packaging/README.md). Run `scripts/package-tarball.sh` after a Release build in `build-linux/`; it emits `dusk-studio-<version>-Linux-<arch>.tar.xz` (a portable program dir + `install.sh`). Requires ImageMagick and `assets/ds-icon.png`.

## Known caveats on Linux

- **JUCE-wayland fork is required at runtime.** The fork has five local commits (XEmbed mapping, X11-on-Wayland fix, peer-creation latch, XEmbed bg fix) on top of plugdata-team's `wayland-juce8` branch. Vanilla upstream JUCE will compile (the `addDefaultFormats` shim in [src/engine/JuceCompat.h](src/engine/JuceCompat.h) abstracts the API split) but will hit the mutter crash on plugin-editor close under GNOME/Wayland. See [CLAUDE.md](CLAUDE.md) for context.
- **Plugin destructors are intentionally leaked at shutdown.** [src/DuskStudioApp.cpp](src/DuskStudioApp.cpp) `leakAllPluginInstancesForShutdown` is a Linux-only workaround for Diva's `__cxa_pure_virtual` abort in `~AM_VST3_ViewInterface`. The OS reclaims memory on process exit.
- **No PipeWire backend without its dev package.** The backend compiles only when pkg-config finds `libpipewire-0.3` at configure time ([CMakeLists.txt:758-764](CMakeLists.txt#L758-L764)), and a configure without it says nothing — you get an ALSA-only binary and notice when the **Audio Device** panel offers one backend instead of two. Install `libpipewire-0.3-dev` first, then configure into a fresh build directory.
- **Compiler warnings.** The vendored Dusk DSP `.cpp` files compiled into Dusk Studio emit shadow/sign-conversion warnings. `DUSKSTUDIO_STRICT_WARNINGS=ON` (`-Werror`) is opt-in but not yet enabled in CI until those are cleaned upstream or wrapped with per-source overrides.

## Reporting build issues

If the build fails, capture:

1. Full CMake configure output (`cmake -S . -B build-linux ...`)
2. Full build output (`cmake --build build-linux -j ...`)
3. `cmake --version`, `gcc --version` or `clang --version`, distro + kernel (`uname -a`, `cat /etc/os-release`)
4. Output of `./build-linux/DuskStudio_artefacts/Release/DuskStudio --version` if you got that far

Open an issue on GitHub or paste into the Patreon support thread.
