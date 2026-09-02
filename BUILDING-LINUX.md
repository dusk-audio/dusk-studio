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
  libsndfile1-dev libsodium-dev libmp3lame-dev \
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
  alsa-lib-devel pipewire-jack-audio-connection-kit-devel pipewire-devel \
  libsndfile-devel libsodium-devel lame-devel \
  lilv-devel suil-devel lv2-devel \
  ladspa-devel \
  libcurl-devel freetype-devel fontconfig-devel \
  libX11-devel libXcomposite-devel libXcursor-devel libXext-devel \
  libXinerama-devel libXrandr-devel libXrender-devel libXScrnSaver-devel \
  webkit2gtk4.0-devel mesa-libGL-devel mesa-libGLU-devel \
  wayland-devel libxkbcommon-devel libdecor-devel
```

libsndfile and libsodium are not optional: all audio file I/O goes through
libsndfile, and the signed SFZ catalog groundwork uses libsodium. Configure
fails when either development package is missing. The PipeWire, LV2-host and
MP3-bounce packages are probed at configure time and quietly drop their feature
when absent; each miss costs you one easy-to-miss line in the configure log, so
read it. On Fedora, take the `pipewire-jack-*` JACK headers rather than
`jack-audio-connection-kit-devel`: the two conflict, and dnf will refuse the
transaction on a PipeWire box.

## Repository layout

Dusk Studio expects four sibling repositories alongside its own checkout:

```
~/projects/
├── dusk-studio/       (this repo)
├── JUCE-wayland/      (plugdata-team fork, branch: wayland-juce8)
├── plugins/           (Dusk Audio plugins, donor DSP)
├── DAF/               (Dusk Audio Framework — native notepad UI)
└── DAF-Widgets/       (Dear ImGui layer for DAF)
```

CMake auto-discovers these. Override with `-DJUCE_PATH=...`, `-DDUSK_PLUGINS_PATH=...`, `-DDAF_PATH=...`, `-DDAF_WIDGETS_PATH=...` if you keep them elsewhere.

### Why the JUCE-wayland fork (Linux-only)

Stock JUCE on Linux uses X11 for top-level windows, which under GNOME / Wayland sessions runs through XWayland. Closing certain plugin editors (Diva, AM_VST3 family) crashes mutter via the `meta_window_unmanage` assertion, taking the whole desktop session down. The [plugdata-team/JUCE wayland-juce8](https://github.com/plugdata-team/JUCE) fork uses libwayland-client + libdecor for top-level windows directly, bypassing XWayland for the main surface.

Cross-platform Dusk Studio source compiles against either upstream JUCE or the fork; the wayland fork is required at runtime on Linux desktops. Mac dev uses upstream JUCE.

`wayland-juce8` is a third-party branch head that moves under you, so the clone below is a dev convenience, not a reproducible input. CI and every release build the Dusk-owned mirror at an immutable tag instead — `dusk-audio/JUCE-wayland`, tag `dusk-wayland-v2`, rev `4d85afa175a45e0b5da11f9211de3ba88705588e` ([release.yml](.github/workflows/release.yml)). Match a release exactly by cloning that tag rather than the branch.

### Clone everything

```bash
cd ~/projects
git clone --recurse-submodules https://github.com/dusk-audio/dusk-studio.git
git clone --branch wayland-juce8 https://github.com/plugdata-team/JUCE.git JUCE-wayland
git clone https://github.com/dusk-audio/dusk-audio-plugins.git plugins
git -C plugins fetch --depth 1 origin 0a1b17f8e9dbecd26bf78dd45704c6c149e4b2ea
git -C plugins checkout --detach FETCH_HEAD
test "$(git -C plugins rev-parse HEAD)" = 0a1b17f8e9dbecd26bf78dd45704c6c149e4b2ea || {
  echo "ERROR: donor checkout did not reach the pinned revision" >&2
  false
}
```

`--recurse-submodules` is required, not tidiness. Dusk Studio carries three: `external/clap`, `external/sfizz`, and `external/vst3sdk`. A clone without them fails configure outright on the CLAP headers (the native CLAP host defaults ON here, [CMakeLists.txt:27-33](CMakeLists.txt#L27-L33), and [CMakeLists.txt:1076-1081](CMakeLists.txt#L1076-L1081) stops the build), and a missing `external/sfizz` costs you the SF2 / multisample instrument with no diagnostic at all ([CMakeLists.txt:1163](CMakeLists.txt#L1163) simply gates on the header being there). Already cloned without them:

```bash
git submodule update --init --recursive
```

The explicit `plugins` target on the third clone is mandatory — the repo is named `dusk-audio-plugins` on GitHub, and `../plugins` is the only sibling directory CMake checks ([CMakeLists.txt:358-367](CMakeLists.txt#L358-L367)). Get it wrong and configure prints a warning rather than failing; the build then produces a recorder with no EQ, compressor, or tape.

The fetch and detached checkout are also mandatory. Dusk Studio consumes a
framework-free compressor core that is present at the revision pinned by all
build and release workflows but is not on the donor repository's current
`main`. When that pin moves, update every workflow and this guide together.

If you also keep an upstream `JUCE/` sibling for cross-OS dev, CMake prefers `JUCE-wayland/` on Linux and falls back to `JUCE/` only if the fork isn't there.

### The native notepad (DAF + Dear ImGui)

The session notepad is Dusk Studio's first native UI window: DAF/DGL for the OpenGL surface, DAF-Widgets for the Dear ImGui layer. Both come from Dusk-owned forks so an upstream rebase can't break a build.

```bash
cd ~/projects
git clone https://github.com/dusk-audio/DAF.git
git -C DAF checkout 66aa1e0365beef70ee097dcacfda4cfc5a25bcee
git -C DAF submodule update --init
git clone https://github.com/dusk-audio/DAF-Widgets.git
git -C DAF-Widgets checkout 798154e874eaaa024371f6076249398b51498142
```

Clone then check out the SHA, rather than building whatever `main` points at today: a branch tip moves and CI fetches these exact SHAs. Both checkouts end up on a detached HEAD, which is what you want here. The submodule step is not optional: DGL pulls the Dusk Pugl fork into `dgl/src/pugl-upstream`.

The pinned Pugl revision is carried by `dusk-pin-5e2621d`, not Pugl's `main`.
Do not delete that branch: a fresh DAF submodule checkout and every CI build
depend on the commit remaining reachable.

The pins live in [.github/actions/clone-daf-stack/action.yml](.github/actions/clone-daf-stack/action.yml), which is the single source of truth for every workflow — read them from there if it ever disagrees with the commands above.

Missing either checkout, `DUSKSTUDIO_ENABLE_NATIVE_UI` defaults to **OFF** and configure says so once, quietly:

```text
-- Native UI: DAF / DAF-Widgets not found - disabled
```

The rest of the app builds and runs normally, but every native view is gone: opening the notepad reports *"Notepad unavailable: built without the native notepad UI"*, the compressor editor, the virtual keyboard and the audio settings panel say the same of themselves, and the startup dialog does not appear. Passing `-DDUSKSTUDIO_ENABLE_NATIVE_UI=ON` with a checkout missing turns that into a configure error instead of a silent downgrade.

That OFF is sticky, because `DUSKSTUDIO_ENABLE_NATIVE_UI` is a **cached** CMake option — the one dependency here that a later reconfigure won't pick up on its own. Configure `build-linux/` before cloning DAF and cloning it afterwards changes nothing on the next configure, and the "not found" line stops printing too, so nothing hints that the notepad is still off. Either configure into a fresh build directory or force the cache, keeping the rest of the flags from [Configure + build](#configure--build) below:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_ENABLE_NATIVE_UI=ON
```

#### Windowing backend

DGL picks X11 or Wayland at configure time, and `-DDGL_BACKEND=` decides which: `auto` (the default) takes X11 whenever the X11 development files are installed and Wayland only when they are absent, `x11` and `wayland` ask for one and fail the configure if its development files are missing. Because one `dgl-opengl3` target serves every consumer in the tree, this is a property of the whole build directory: it is X11 or Wayland, not both.

Leave it at `auto` for the app. The notepad is a native child window placed inside the JUCE main window, and Wayland has no window embedding — a `-DDGL_BACKEND=wayland` build of the app therefore refuses to open the notepad, reporting *Unable to embed session notepad with the current display backend*. The flag is there for the GUI tower's own build directories, which run standalone framework windows on a real Wayland session:

```bash
cmake -S . -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDUSKSTUDIO_BUILD_GUI_SPIKE=ON -DDGL_BACKEND=wayland
```

## Configure + build

From the Dusk Studio directory:

```bash
cd ~/projects/dusk-studio
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j6
```

First configure pulls in JUCE's CMake helpers and may take a minute. Subsequent configures are fast.

Six jobs is a conservative default for a 16 GB-class machine. Large C++
translation units can use roughly 500 MB each, so scale the number to free
memory rather than core count: drop to `-j2` on a 4 GB Raspberry Pi, or raise
it on a 32 GB-class machine. The developer and screenshot helpers accept an
explicit override, for example `DUSK_JOBS=12 scripts/dev.sh`.

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
cmake --build build-linux-debug -j6
```

### Overriding paths (if not using the sibling layout)

```bash
cmake -S . -B build-linux \
  -DJUCE_PATH=/some/other/JUCE \
  -DDUSK_PLUGINS_PATH=/some/other/plugins \
  -DDAF_PATH=/some/other/DAF \
  -DDAF_WIDGETS_PATH=/some/other/DAF-Widgets
```

## Tests

```bash
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Release -DDUSKSTUDIO_BUILD_TESTS=ON
cmake --build build-tests --target dusk-studio-tests -j6
ctest --test-dir build-tests --output-on-failure
```

Use a separate `build-tests/` directory so the two configurations don't fight over CMake cache state.

### Under AddressSanitizer + UBSan

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDUSKSTUDIO_BUILD_TESTS=ON -DDUSKSTUDIO_ENABLE_ASAN=ON
cmake --build build-asan --target dusk-studio-tests -j6
ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0" \
  ctest --test-dir build-asan --output-on-failure
```

CI runs this nightly via [.github/workflows/linux-sanitizer.yml](.github/workflows/linux-sanitizer.yml).

## Headless self-test

Drives the synthetic DSP pipeline without opening the GUI:

```bash
scripts/run-selftest-xvfb.sh \
  ./build-linux/DuskStudio_artefacts/Release/DuskStudio
```

Useful for confirming the audio engine wires up correctly without needing to drive the UI.

## Audio backend selection

Dusk Studio ships two backends of its own, both speaking to the system directly rather than through JUCE. Pick from the **Audio Device** panel inside Dusk Studio, where they appear as **PipeWire** and **ALSA**.

- **PipeWire** ([src/engine/pipewire/](src/engine/pipewire/)) — a single `pw_filter` node on the graph; every Sink / Source node is listed as its own device. Correct graph latency and a client that shows up as "Dusk Studio" instead of a generic JACK name. Registered first ([src/engine/device/DeviceManager.cpp:204-218](src/engine/device/DeviceManager.cpp#L204-L218)), so it wins the first-run pick.
- **ALSA** ([src/engine/alsa/](src/engine/alsa/)) — direct hardware access, no graph hops. What you get when PipeWire isn't running, and what to pick when you want the interface to yourself.

On a machine with no saved settings the first backend that enumerates any device is selected ([DeviceManager.cpp:227-236](src/engine/device/DeviceManager.cpp#L227-L236)), which is PipeWire when it's built in and the graph is up. After that your saved choice wins: the stored device blob names its backend and that lookup runs first ([DeviceManager.cpp:426-430](src/engine/device/DeviceManager.cpp#L426-L430)).

There is no JACK backend to pick — no JACK device type is registered, so nothing JACK-shaped appears in the panel. Dusk Studio used to reach PipeWire through JUCE's JACK path over the pipewire-jack shim, and the native backend replaced it; `juce_audio_devices` still compiles (pulled in transitively by `juce_audio_utils`, [CMakeLists.txt:1316-1320](CMakeLists.txt#L1316-L1320)) with `JUCE_JACK=1`, which is why the JACK development headers stay a build dependency. To feed other applications while the PipeWire backend is active, patch Dusk Studio inside the graph with qpwgraph or Helvum; on the ALSA backend there is no graph to patch.

The Dusk Studio-native ALSA backend handles USB hot-unplug by surfacing the device error to the engine, which finalises any in-flight take. Details in [MANUAL.md](MANUAL.md#audio-device-disconnected-mid-session).

## Out-of-process plugin host (opt-in, all platforms)

```bash
DUSKSTUDIO_USE_OOP_PLUGINS=1 ./build-linux/DuskStudio_artefacts/Release/DuskStudio
```

Routes new plugin loads through the `dusk-studio-plugin-host` child process so a misbehaving plugin can't take down the host. Implemented on all three OSes (Linux via `memfd_create` + `futex`, macOS via Mach ports, Windows via named pipes). In-process is the default for the lowest editor latency; set `DUSKSTUDIO_USE_OOP_PLUGINS=1` to opt into the crash-isolating sandbox. Plugin **scanning** is always sandboxed regardless of this flag.

## Packaging the Linux tarball

See [packaging/README.md](packaging/README.md). Run `scripts/package-tarball.sh` after a Release build in `build-linux/`; it emits `dusk-studio-<version>-Linux-<arch>.tar.xz` (a portable program dir + `install.sh`).

## Known caveats on Linux

- **JUCE-wayland fork is required at runtime.** The fork has five local commits (XEmbed mapping, X11-on-Wayland fix, peer-creation latch, XEmbed bg fix) on top of plugdata-team's `wayland-juce8` branch. Vanilla upstream JUCE will compile (the `addDefaultFormats` shim in [src/engine/JuceCompat.h](src/engine/JuceCompat.h) abstracts the API split) but will hit the mutter crash on plugin-editor close under GNOME/Wayland. See [CLAUDE.md](CLAUDE.md) for context.
- **Plugin destructors are intentionally leaked at shutdown.** [src/DuskStudioApp.cpp](src/DuskStudioApp.cpp) `leakAllPluginInstancesForShutdown` is a Linux-only workaround for Diva's `__cxa_pure_virtual` abort in `~AM_VST3_ViewInterface`. The OS reclaims memory on process exit.
- **No PipeWire backend without its dev package.** The backend compiles only when pkg-config finds `libpipewire-0.3` 0.3.48 or newer at configure time ([CMakeLists.txt:783-789](CMakeLists.txt#L783-L789)). The configure log says which way it went — `Native PipeWire backend: libpipewire-0.3 <version> found - enabled`, or `... not found - disabled, ALSA only` — and the symptom of a miss is an ALSA-only **Audio Device** panel. Install `libpipewire-0.3-dev` and configure again; the probe re-runs every time, so the same build directory is fine. Pass `-DDUSKSTUDIO_REQUIRE_PIPEWIRE=ON` (what CI and the release build use) to turn the miss into a configure error instead.
- **Compiler warnings.** The vendored Dusk DSP `.cpp` files compiled into Dusk Studio emit shadow/sign-conversion warnings. `DUSKSTUDIO_STRICT_WARNINGS=ON` (`-Werror`) is opt-in but not yet enabled in CI until those are cleaned upstream or wrapped with per-source overrides.

## Reporting build issues

If the build fails, capture:

1. Full CMake configure output (`cmake -S . -B build-linux ...`)
2. Full build output (`cmake --build build-linux -j6 ...`)
3. `cmake --version`, `gcc --version` or `clang --version`, distro + kernel (`uname -a`, `cat /etc/os-release`)
4. Output of `./build-linux/DuskStudio_artefacts/Release/DuskStudio --version` if you got that far

Open an issue on GitHub or paste into the Patreon support thread.
