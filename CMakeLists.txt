cmake_minimum_required(VERSION 3.22)

# macOS deployment target. Must be set BEFORE project() so the C/C++
# compiler probes pick it up and the resulting binary's
# LC_BUILD_VERSION reflects it (Gatekeeper rejects apps with no
# LSMinimumSystemVersion or with a target older than the running OS
# claims). 11.0 (Big Sur) covers every Mac released since 2013 with
# a supported macOS upgrade path, matches the JUCE 8 minimum, and
# avoids the Catalina notarization headaches.
if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS deployment target")
endif()

# Version is sourced from the top-level VERSION file so packaging
# scripts (AppImage, deb/rpm, MSI, DMG) and AppStream metadata see the
# same value as the binary. scripts/bump-version.sh writes both
# VERSION and the AppStream <release> entry; do NOT edit the project()
# version literal here.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DUSKSTUDIO_VERSION_RAW)
string(STRIP "${DUSKSTUDIO_VERSION_RAW}" DUSKSTUDIO_VERSION)
project(DuskStudio VERSION ${DUSKSTUDIO_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC C runtime: static (/MT) for the whole tree. Vendored sfizz forces
# the static runtime on itself (cmake/SfizzConfig.cmake), so DuskStudio +
# JUCE must match or the link fails with LNK2038 "RuntimeLibrary mismatch
# (MT_StaticRelease vs MD_DynamicRelease)". Static also makes the MSI
# self-contained — no vc_redist dependency on the target machine. Relies
# on CMP0091=NEW (guaranteed by cmake_minimum_required 3.22).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Emit compile_commands.json so clangd / editor tooling resolves JUCE +
# donor include paths. Symlink it to the repo root once (see
# scripts/dev.sh) and clangd picks it up automatically.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use ccache when present — caches object files across rebuilds so a
# touched header doesn't force a full sfizz/absl/JUCE recompile. No-op
# when ccache isn't installed.
find_program(DUSKSTUDIO_CCACHE ccache)
if(DUSKSTUDIO_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${DUSKSTUDIO_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${DUSKSTUDIO_CCACHE}")
    message(STATUS "ccache enabled: ${DUSKSTUDIO_CCACHE}")
endif()

# JUCE path resolution. Per-OS defaults so a developer toggling between
# Mac (upstream JUCE) and Linux (plugdata-team JUCE-wayland fork, required
# for the wayland-juce8 features) doesn't have to pass -DJUCE_PATH each time.
# Priority:
#   1. -DJUCE_PATH (explicit override)
#   2. Linux: sibling ../JUCE-wayland (wayland fork)
#   3. Any OS: sibling ../JUCE (upstream)
#   4. external/JUCE submodule
# The wayland fork has its own XEmbed + addDefaultFormatsToManager API that
# Dusk Studio's source assumes on Linux; see [src/engine/JuceCompat.h](src/engine/JuceCompat.h)
# for the API-split shim.
if(DEFINED JUCE_PATH)
    set(DUSKSTUDIO_JUCE_SOURCE_DIR "${JUCE_PATH}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE-wayland/CMakeLists.txt")
    set(DUSKSTUDIO_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE-wayland")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt")
    set(DUSKSTUDIO_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    set(DUSKSTUDIO_JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE")
else()
    message(FATAL_ERROR
        "JUCE not found. Options:\n"
        "  1. Set -DJUCE_PATH=/path/to/JUCE\n"
        "  2. Clone JUCE to ../JUCE (sibling) — or ../JUCE-wayland on Linux\n"
        "  3. Add as submodule: git submodule add https://github.com/juce-framework/JUCE.git external/JUCE")
endif()

message(STATUS "Using JUCE from: ${DUSKSTUDIO_JUCE_SOURCE_DIR}")

# Linux-only: verify our local XEmbed mapping fix is present in the JUCE checkout.
# Stock JUCE 8.0.10 ties the host container's X11 mapping to the embedded
# plugin's _XEMBED_INFO flag, which leaves pure-xcb VST3s (e.g. MininnDrum) as
# blank rectangles. The fix splits WindowMapper by Role; the marker below is in
# our local commit. Detect drift if JUCE is ever updated without re-applying.
#
# Skippable via -DDUSKSTUDIO_SKIP_FORK_CHECK=ON for CI / test builds that use a
# vanilla upstream JUCE clone (no plugin editors involved, so the XEmbed patch
# is irrelevant). Never set this for a distribution build — pure-xcb VST3
# plugins will render blank without the patch.
option(DUSKSTUDIO_SKIP_FORK_CHECK
       "Skip the JUCE-wayland XEmbed patch enforcement (CI / tests only)" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DUSKSTUDIO_SKIP_FORK_CHECK)
    set(_duskstudio_xembed_file
        "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/juce_gui_extra/native/juce_XEmbedComponent_linux.cpp")
    if(NOT EXISTS "${_duskstudio_xembed_file}")
        message(FATAL_ERROR "Expected JUCE source not found: ${_duskstudio_xembed_file}")
    endif()
    file(READ "${_duskstudio_xembed_file}" _duskstudio_xembed_src)
    if(NOT _duskstudio_xembed_src MATCHES "enum class Role { host, client }")
        message(FATAL_ERROR
            "Dusk Studio requires a patched JUCE: the XEmbed host-mapping fix is missing in\n"
            "  ${_duskstudio_xembed_file}\n"
            "Without it, pure-xcb VST3 plugins (e.g. MininnDrum) render as a blank\n"
            "rectangle. Re-apply the local commit titled\n"
            "  \"XEmbed: stop tying host-window mapping to client _XEMBED_INFO\"\n"
            "in your JUCE checkout, or pull a JUCE branch that contains it.\n"
            "(CI / test builds: pass -DDUSKSTUDIO_SKIP_FORK_CHECK=ON to bypass.)")
    endif()
    unset(_duskstudio_xembed_file)
    unset(_duskstudio_xembed_src)
endif()

# Detect plugdata-team JUCE-wayland's free juce::addDefaultFormatsToManager
# function at configure time. The fork added it + marked the member
# = delete; upstream JUCE only has the member. JuceCompat.h picks the
# right API via the DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS preprocessor define.
# Done at configure time because SFINAE on the free function fails:
# the qualified name lookup happens at template definition, not
# substitution, so calling code wouldn't compile against upstream.
set(DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS OFF)
set(_duskstudio_apf_helpers
    "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/juce_audio_processors/format/juce_AudioPluginFormatManagerHelpers.h")
if(EXISTS "${_duskstudio_apf_helpers}")
    file(READ "${_duskstudio_apf_helpers}" _duskstudio_apf_src)
    if(_duskstudio_apf_src MATCHES "addDefaultFormatsToManager")
        set(DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS ON)
        message(STATUS "JUCE has free juce::addDefaultFormatsToManager (wayland fork)")
    endif()
    unset(_duskstudio_apf_src)
endif()
unset(_duskstudio_apf_helpers)

# Detect the wayland fork's WaylandWindowSystem / WaylandSymbols. The
# fork integrates libwayland-client + libdecor directly for top-level
# windowing; upstream JUCE has no such symbols. Dusk Studio's
# PlatformWindowing_Linux.cpp uses them to drive the X11-vs-Wayland
# peer-creation latch + the requestFocusOnMainWaylandSurface roundtrip
# yield. CI builds against upstream JUCE (no Wayland symbols) need
# those code paths stubbed.
set(DUSKSTUDIO_JUCE_HAS_WAYLAND OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_duskstudio_juce_wayland_dir "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/juce_gui_basics/native")
    file(GLOB _duskstudio_juce_wayland_files "${_duskstudio_juce_wayland_dir}/*.h")
    foreach(_f ${_duskstudio_juce_wayland_files})
        file(READ "${_f}" _duskstudio_juce_wayland_src)
        if(_duskstudio_juce_wayland_src MATCHES "class +WaylandWindowSystem")
            set(DUSKSTUDIO_JUCE_HAS_WAYLAND ON)
            message(STATUS "JUCE has WaylandWindowSystem (wayland fork): ${_f}")
            break()
        endif()
    endforeach()
    unset(_duskstudio_juce_wayland_files)
    unset(_duskstudio_juce_wayland_src)
    unset(_duskstudio_juce_wayland_dir)
endif()

add_subdirectory(${DUSKSTUDIO_JUCE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

# App icon. JUCE bakes the PNG into platform-specific resources at
# build time (.ico on Windows, .icns on macOS, embedded data block on
# Linux). Same file is used for both BIG and SMALL - JUCE downscales
# internally as needed. The conditional keeps a fresh checkout
# building when assets/ds-icon.png hasn't been added yet.
set(DUSKSTUDIO_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/ds-icon.png")
if(EXISTS "${DUSKSTUDIO_ICON_PATH}")
    set(DUSKSTUDIO_ICON_ARGS
        ICON_BIG   "${DUSKSTUDIO_ICON_PATH}"
        ICON_SMALL "${DUSKSTUDIO_ICON_PATH}")
    message(STATUS "Dusk Studio app icon: ${DUSKSTUDIO_ICON_PATH}")
else()
    set(DUSKSTUDIO_ICON_ARGS "")
    message(STATUS "Dusk Studio app icon not found at ${DUSKSTUDIO_ICON_PATH} - building without one")
endif()

juce_add_gui_app(DuskStudio
    PRODUCT_NAME       "DuskStudio"
    COMPANY_NAME       "Marc Korte"
    BUNDLE_ID          "audio.dusk.studio"
    VERSION            ${PROJECT_VERSION}
    ${DUSKSTUDIO_ICON_ARGS}
    NEEDS_WAYLAND      TRUE
    HARDENED_RUNTIME_ENABLED TRUE
    HARDENED_RUNTIME_OPTIONS
        com.apple.security.cs.allow-jit
        com.apple.security.cs.allow-unsigned-executable-memory
        com.apple.security.cs.disable-library-validation
        com.apple.security.device.audio-input
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Dusk Studio needs microphone access to record audio from your inputs.")

# Runtime app icon. macOS gets it from the .icns in the .app bundle and
# Windows from the .ico baked into the PE resource (both via ICON_BIG above),
# but Linux has no automatic _NET_WM_ICON wiring — JUCE 8 doesn't call
# setIcon on the main peer for you. Embed the PNG and call setIcon
# explicitly in DuskStudioApp::MainWindow so the WM / taskbar / Alt-Tab
# render the brand icon regardless of install layout.
if(EXISTS "${DUSKSTUDIO_ICON_PATH}")
    juce_add_binary_data(DuskStudioBinaryIcon SOURCES "${DUSKSTUDIO_ICON_PATH}")
    target_link_libraries(DuskStudio PRIVATE DuskStudioBinaryIcon)
endif()

# macOS code signing. Ad-hoc by default (works for local dev launch +
# satisfies the runtime-loadable plugin requirement that the host be
# at least ad-hoc signed). For distribution: set
#   -DDUSKSTUDIO_CODESIGN_IDENTITY="Developer ID Application: <name> (TEAMID)"
# at configure time and the post-build step uses that identity instead.
# Notarization is still a manual step (xcrun notarytool submit + staple)
# because it needs an Apple ID + app-specific password the build can't
# safely embed.
if(APPLE)
    set(DUSKSTUDIO_CODESIGN_IDENTITY "-" CACHE STRING "macOS codesign identity. '-' = ad-hoc (default). Override for distribution builds.")
    # Inject LSMinimumSystemVersion into the bundled Info.plist. JUCE 8
    # builds the plist from juce_add_gui_app args but doesn't expose this
    # key directly; without it, Gatekeeper / App Store / older macOS
    # launchers will reject the bundle or run it under broken
    # compatibility shims. plutil -replace -string is idempotent (it
    # overwrites an existing key, or adds it if missing) so reruns
    # don't accumulate. Must run BEFORE codesign so the modified
    # Info.plist hashes are sealed into the signature.
    add_custom_command(TARGET DuskStudio POST_BUILD
        COMMAND plutil -replace LSMinimumSystemVersion -string
                "${CMAKE_OSX_DEPLOYMENT_TARGET}"
                "$<TARGET_BUNDLE_DIR:DuskStudio>/Contents/Info.plist"
        # Pass --entitlements pointing at JUCE's generated plist so the
        # allow-jit / allow-unsigned-executable-memory / disable-library-
        # validation flags stay attached. Without this, the resigning
        # below STRIPS the entitlements JUCE already applied; hardened
        # runtime then blocks dlopen / JIT in 3rd-party VSTi plugins
        # and they fail to load with "Plugin was located, but could
        # not be opened". --deep dropped too (deprecated; JUCE has
        # already signed inner Mach-Os).
        COMMAND codesign --force --options runtime
                          --entitlements "${CMAKE_CURRENT_BINARY_DIR}/DuskStudio_artefacts/JuceLibraryCode/DuskStudio.entitlements"
                          --sign "${DUSKSTUDIO_CODESIGN_IDENTITY}"
                          "$<TARGET_BUNDLE_DIR:DuskStudio>"
        COMMENT "patch Info.plist + codesign DuskStudio.app (identity='${DUSKSTUDIO_CODESIGN_IDENTITY}')"
        VERBATIM)
endif()

# Dusk Audio plugins repo — donor for the analog DSP cores (4K EQ filter chain,
# FET/Opto compressor, Pultec EQ, tape saturation, console saturation, plus the
# AnalogEmulation library). Header-only cores are picked up via the include
# paths below; editing a header in the plugins repo automatically rebuilds
# the affected Dusk Studio translation units. No copy / submodule bump required.
#
# Resolution priority:
#   1. -DDUSK_PLUGINS_PATH (explicit override)
#   2. sibling ../plugins-main — a permanent worktree pinned to plugins-`main`.
#      Use this when the working ../plugins checkout is on a feature branch
#      that may not have the donor API Dusk Studio expects. Create with:
#          cd ../plugins && git worktree add ../plugins-main main
#   3. sibling ../plugins (the usual checkout)
if(DEFINED DUSK_PLUGINS_PATH)
    message(STATUS "Using Dusk plugins from: ${DUSK_PLUGINS_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../plugins-main/plugins/shared")
    set(DUSK_PLUGINS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../plugins-main")
    message(STATUS "Using Dusk plugins from sibling worktree (plugins-main): ${DUSK_PLUGINS_PATH}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../plugins/plugins/shared")
    set(DUSK_PLUGINS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../plugins")
    message(STATUS "Using Dusk plugins from sibling directory: ${DUSK_PLUGINS_PATH}")
else()
    message(WARNING
        "Dusk plugins repo not found. Dusk Studio will build but DSP cores will be "
        "unavailable until you set -DDUSK_PLUGINS_PATH=/path/to/plugins or clone "
        "the plugins repo to ../plugins (sibling directory).")
endif()

target_sources(DuskStudio PRIVATE
    src/Main.cpp
    src/DuskStudioApp.cpp
    src/engine/AudioEngine.cpp
    src/engine/AudioWorkerPool.cpp
    src/engine/BounceEngine.cpp
    src/engine/DpAligner.cpp
    src/engine/DpImporter.cpp
    src/engine/FileImporter.cpp
    src/engine/MasteringPlayer.cpp
    src/engine/MidiClockEmitter.cpp
    src/engine/MidiSyncReceiver.cpp
    src/engine/MidiTimeCodeReceiver.cpp
    src/engine/McuReceiver.cpp
    src/engine/McuController.cpp
    src/engine/MidiTimeCodeEmitter.cpp
    src/engine/PlaybackEngine.cpp
    src/engine/PluginManager.cpp
    src/engine/PluginSlot.cpp
    src/engine/RecordManager.cpp
    src/dsp/AuxLaneStrip.cpp
    src/dsp/BusStrip.cpp
    src/dsp/BrickwallLimiter.cpp
    src/dsp/ChannelStrip.cpp
    src/dsp/HardwareInsertSlot.cpp
    src/dsp/LoudnessMeter.cpp
    src/dsp/MasterBus.cpp
    src/dsp/MasteringChain.cpp
    src/dsp/MasteringDigitalEq.cpp
    src/dsp/Metronome.cpp
    src/session/MarkerEditActions.cpp
    src/session/RecentSessions.cpp
    src/session/RegionEditActions.cpp
    src/dsp/ChordAnalyzer.cpp
    src/session/MidiBindings.cpp
    src/session/Session.cpp
    src/session/SessionSerializer.cpp
    src/session/SessionTemplates.cpp
    src/util/CrashHandler.cpp
    src/ui/AnalogVuMeter.cpp
    src/ui/AppConfig.cpp
    src/ui/AudioRegionEditor.cpp
    src/ui/AudioSettingsPanel.cpp
    src/ui/AuxLaneComponent.cpp
    src/ui/DuskAlerts.cpp
    src/ui/DuskComboBox.cpp
    src/ui/DuskContextMenu.cpp
    src/ui/CursorOverlay.cpp
    src/ui/DuskFileBrowser.cpp
    src/ui/EditCursors.cpp
    src/ui/AuxView.cpp
    src/ui/BusComponent.cpp
    src/ui/BounceDialog.cpp
    src/ui/ChannelCompEditor.cpp
    src/ui/ChannelEqEditor.cpp
    src/ui/ChannelStripComponent.cpp
    src/ui/CompMeterStrip.cpp
    src/ui/SelfTestPanel.cpp
    src/ui/StartupDialog.cpp
    src/engine/AudioPipelineSelfTest.cpp
    src/ui/ConsoleView.cpp
    src/ui/DimOverlay.cpp
    src/ui/HardwareInsertEditor.cpp
    src/ui/ImportTargetPicker.cpp
    src/ui/MainComponent.cpp
    src/ui/ScreenshotCapture.cpp
    src/ui/MultiImportTargetPicker.cpp
    src/ui/MasteringEqEditor.cpp
    src/ui/MasteringLimiterEditor.cpp
    src/ui/MasteringView.cpp
    src/ui/MasterStripComponent.cpp
    src/ui/MidiBindingsPanel.cpp
    src/ui/PianoRollComponent.cpp
    src/ui/PluginPickerHelpers.cpp
    src/ui/PluginPickerPanel.cpp
    src/ui/PluginScanModal.cpp
    src/ui/SystemStatusBar.cpp
    src/ui/EditModeToolbar.cpp
    src/ui/TapeStrip.cpp
    src/ui/TransportBar.cpp
    src/ui/VirtualKeyboardComponent.cpp
    src/ui/WindowState.cpp)

# Cross-platform window-management primitives. One implementation per
# platform; callsites #include only PlatformWindowing.h and stay
# platform-agnostic. Mac and Windows impls are stubs today (see
# PlatformWindowing.h for the eventual per-platform mechanisms) but
# the files exist so the fix slot is obvious when the equivalent bugs
# surface there.
if(APPLE)
    target_sources(DuskStudio PRIVATE src/ui/PlatformWindowing_Mac.mm)
elseif(WIN32)
    target_sources(DuskStudio PRIVATE src/ui/PlatformWindowing_Windows.cpp)
else()
    target_sources(DuskStudio PRIVATE src/ui/PlatformWindowing_Linux.cpp)
endif()

# ASIO (Windows only). Steinberg's ASIO SDK can't be bundled because of its
# licence, so the build looks for the SDK on disk. Resolution order:
#   1. -DASIOSDK_PATH=<dir>           (CMake cache override)
#   2. $ASIOSDK_PATH environment var  (CI / shell config)
#   3. sibling directory ../asiosdk   (mirrors the JUCE / plugins pattern)
# The SDK's `common/` folder must contain iasiodrv.h. When found, JUCE_ASIO
# is set so juce_audio_devices' juce_ASIO_windows.cpp compiles in the
# driver class; the AudioEngine then registers ASIO ahead of WASAPI so it
# becomes the default backend (see AudioEngine ctor).
#
# ASIO is the only low-latency path JUCE offers on Windows (no WDM-KS), so a
# shipping build MUST have it — a WASAPI-only Windows binary is what makes
# pro-audio users walk. DUSKSTUDIO_REQUIRE_ASIO turns a missing SDK into a
# hard configure error. It auto-enables whenever a release-class build is
# possible: single-config Ninja/Make Release (CMAKE_BUILD_TYPE), and the
# Visual Studio multi-config generator whenever Release is among the
# buildable configs (the common case). A deliberate ASIO-less dev build
# passes -DDUSKSTUDIO_REQUIRE_ASIO=OFF; without the SDK it then warns and
# falls back to WASAPI exclusive / shared.
# Tri-state so an explicit -DDUSKSTUDIO_REQUIRE_ASIO=OFF is distinguishable
# from the unset default: "" = auto (release-class builds fail closed),
# ON = always require, OFF = never require (deliberate ASIO-less dev build).
set(DUSKSTUDIO_REQUIRE_ASIO "" CACHE STRING
    "Require the ASIO SDK on Windows: ON / OFF / empty=auto (release fails closed)")
# Validate up front: only "", ON, OFF are meaningful. Without this, a typo
# like -DDUSKSTUDIO_REQUIRE_ASIO=yes falls through the "NOT ... OFF" branch and
# is silently treated as "require ASIO". Fail loud instead.
if(NOT DUSKSTUDIO_REQUIRE_ASIO STREQUAL ""
   AND NOT DUSKSTUDIO_REQUIRE_ASIO STREQUAL "ON"
   AND NOT DUSKSTUDIO_REQUIRE_ASIO STREQUAL "OFF")
    message(FATAL_ERROR
        "DUSKSTUDIO_REQUIRE_ASIO must be ON, OFF, or empty (auto); got "
        "'${DUSKSTUDIO_REQUIRE_ASIO}'")
endif()
if(WIN32)
    set(_asio_required ${DUSKSTUDIO_REQUIRE_ASIO})
    # Decide whether a release-class build is possible with this generator.
    # Single-config (Ninja/Make): the type is fixed at configure time, so read
    # CMAKE_BUILD_TYPE. Multi-config (Visual Studio): the type is chosen at
    # BUILD time and CMAKE_BUILD_TYPE is empty, so a genex like $<CONFIG:Release>
    # can't drive a configure-time message(FATAL_ERROR). Instead, if Release is
    # among the buildable configs we fail closed — a VS build must not be able
    # to silently produce a WASAPI-only shipping binary. An explicit
    # -DDUSKSTUDIO_REQUIRE_ASIO=OFF opts out (deliberate ASIO-less dev build)
    # and must win over this auto-detection.
    if(NOT DUSKSTUDIO_REQUIRE_ASIO STREQUAL "OFF")
        get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
        if(_is_multi_config)
            if("Release"        IN_LIST CMAKE_CONFIGURATION_TYPES
               OR "RelWithDebInfo" IN_LIST CMAKE_CONFIGURATION_TYPES
               OR "MinSizeRel"     IN_LIST CMAKE_CONFIGURATION_TYPES)
                set(_asio_required ON)
            endif()
        else()
            # Normalise to lowercase so a lowercase -DCMAKE_BUILD_TYPE=release
            # still trips the release-class fail-closed (STREQUAL is
            # case-sensitive and would otherwise miss it).
            string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type_lc)
            if(_build_type_lc STREQUAL "release"
               OR _build_type_lc STREQUAL "relwithdebinfo"
               OR _build_type_lc STREQUAL "minsizerel")
                set(_asio_required ON)
            endif()
        endif()
    endif()

    set(_asio_candidate "")
    if(DEFINED ASIOSDK_PATH AND EXISTS "${ASIOSDK_PATH}/common/iasiodrv.h")
        set(_asio_candidate "${ASIOSDK_PATH}")
    elseif(DEFINED ENV{ASIOSDK_PATH} AND EXISTS "$ENV{ASIOSDK_PATH}/common/iasiodrv.h")
        set(_asio_candidate "$ENV{ASIOSDK_PATH}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk/common/iasiodrv.h")
        set(_asio_candidate "${CMAKE_CURRENT_SOURCE_DIR}/../asiosdk")
    endif()

    if(NOT "${_asio_candidate}" STREQUAL "")
        message(STATUS "Dusk Studio: ASIO SDK found at ${_asio_candidate} - enabling JUCE_ASIO")
        target_include_directories(DuskStudio PRIVATE "${_asio_candidate}/common")
        target_compile_definitions(DuskStudio PRIVATE JUCE_ASIO=1)
    elseif(_asio_required)
        message(FATAL_ERROR
            "Dusk Studio: ASIO SDK is required for this build but was not found.\n"
            "  A Windows release binary without ASIO is not shippable (WASAPI-only latency).\n"
            "  Fix one of:\n"
            "    - download Steinberg's ASIO SDK and pass -DASIOSDK_PATH=<dir> (its common/ must hold iasiodrv.h)\n"
            "    - set the ASIOSDK_PATH environment variable, or place the SDK at ../asiosdk\n"
            "    - for a deliberate dev build without ASIO, pass -DDUSKSTUDIO_REQUIRE_ASIO=OFF and a non-release CMAKE_BUILD_TYPE")
    else()
        message(WARNING
            "Dusk Studio: ASIO SDK not found - falling back to WASAPI exclusive/shared. "
            "Fine for dev builds; shipping Windows builds must include ASIO (set -DASIOSDK_PATH=...).")
    endif()
endif()

# Dusk Studio-owned ALSA backend - a juce::AudioIODevice + AudioIODeviceType pair
# written from scratch using Ardour's libs/backends/alsa/ as a reference for
# patterns (xrun recovery, sw_params choices, format priority, channel-mask
# handling) but with no copied code. Replaces JUCE's stock ALSA backend,
# which is structurally insufficient for hardware-direct studio use across
# the long tail of audio interfaces.
#
# Linux-only: the implementation depends on <alsa/asoundlib.h>. macOS and
# Windows builds skip it and fall back to the audio backends JUCE provides
# (CoreAudio / WASAPI). The AudioEngine constructor's registration is
# similarly guarded by #if defined(__linux__).
if(UNIX AND NOT APPLE)
    target_sources(DuskStudio PRIVATE
        src/engine/alsa/AlsaAudioIODevice.cpp
        src/engine/alsa/AlsaAudioIODeviceType.cpp
        src/engine/alsa/AlsaPerformanceTest.cpp)
    target_link_libraries(DuskStudio PRIVATE asound X11)
    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_ALSA=1)
endif()

# Out-of-process plugin hosting: shared IPC primitives + the parent-side
# connection class + a self-test driver. The dusk-studio-plugin-host
# child binary lives next to Dusk Studio in the build output.
#
# Supported platforms:
#   Linux   : memfd_create + futex + socketpair + SCM_RIGHTS
#   Windows : CreateFileMapping + WaitOnAddress + named pipe
#   macOS   : shm_open + os_sync_wait_on_address + socketpair + SCM_RIGHTS
#             (requires macOS 14.4 — os_sync_wait_on_address shared
#             addresses were added in Sonoma 14.4; older systems fall
#             back to the in-process path because DUSKSTUDIO_HAS_OOP_PLUGINS
#             stays undefined.)
# DUSKSTUDIO_HAS_OOP_PLUGINS gates every #if branch in PluginSlot that
# routes through RemotePluginConnection.
set(_duskstudio_oop_enabled FALSE)
if(UNIX AND NOT APPLE)
    set(_duskstudio_oop_enabled TRUE)
    set(_duskstudio_ipc_platform_sources
        src/engine/ipc/platform/IpcChannel_Linux.cpp
        src/engine/ipc/platform/IpcProcess_Linux.cpp
        src/engine/ipc/platform/IpcShm_Linux.cpp
        src/engine/ipc/platform/IpcSync_Linux.cpp)
    set(_duskstudio_ipc_child_platform_sources
        src/engine/ipc/platform/IpcChannel_Linux.cpp
        src/engine/ipc/platform/IpcShm_Linux.cpp
        src/engine/ipc/platform/IpcSync_Linux.cpp)
elseif(WIN32)
    set(_duskstudio_oop_enabled TRUE)
    set(_duskstudio_ipc_platform_sources
        src/engine/ipc/platform/IpcChannel_Windows.cpp
        src/engine/ipc/platform/IpcProcess_Windows.cpp
        src/engine/ipc/platform/IpcShm_Windows.cpp
        src/engine/ipc/platform/IpcSync_Windows.cpp)
    set(_duskstudio_ipc_child_platform_sources
        src/engine/ipc/platform/IpcChannel_Windows.cpp
        src/engine/ipc/platform/IpcShm_Windows.cpp
        src/engine/ipc/platform/IpcSync_Windows.cpp)
elseif(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER_EQUAL 14.4)
    set(_duskstudio_oop_enabled TRUE)
    set(_duskstudio_ipc_platform_sources
        src/engine/ipc/platform/IpcChannel_Mac.cpp
        src/engine/ipc/platform/IpcProcess_Mac.cpp
        src/engine/ipc/platform/IpcShm_Mac.cpp
        src/engine/ipc/platform/IpcSync_Mac.cpp)
    set(_duskstudio_ipc_child_platform_sources
        src/engine/ipc/platform/IpcChannel_Mac.cpp
        src/engine/ipc/platform/IpcShm_Mac.cpp
        src/engine/ipc/platform/IpcSync_Mac.cpp)
endif()

if(_duskstudio_oop_enabled)

    target_sources(DuskStudio PRIVATE
        src/engine/ipc/RemotePluginConnection.cpp
        src/engine/ipc/IpcSelfTest.cpp
        ${_duskstudio_ipc_platform_sources})
    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_OOP_PLUGINS=1)
    if(WIN32)
        # WaitOnAddress / WakeByAddressSingle live in synchronization.lib
        # (re-exported by API-Set "api-ms-win-core-synch-l1-2-0.dll" on
        # Win8+). Link explicitly on the parent so the IPC sync layer
        # resolves; the child binary links it too below.
        target_link_libraries(DuskStudio PRIVATE synchronization)
    endif()

    # dusk-studio-plugin-host is a JUCE console app so it can pull in the
    # AudioPluginFormatManager + AudioPluginInstance machinery and the
    # gui_extra modules for plugin-editor embedding.
    juce_add_console_app(dusk-studio-plugin-host
        PRODUCT_NAME "dusk-studio-plugin-host"
        COMPANY_NAME "Marc Korte"
        BUNDLE_ID    "audio.dusk.studio.pluginhost")
    target_sources(dusk-studio-plugin-host PRIVATE
        src/engine/ipc/PluginHostMain.cpp
        ${_duskstudio_ipc_child_platform_sources})
    target_compile_features (dusk-studio-plugin-host PRIVATE cxx_std_17)
    target_compile_definitions (dusk-studio-plugin-host PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LV2=1
        # Match the main app: the child must host/scan AU on macOS too,
        # otherwise --scan can't sandbox AudioUnit discovery there.
        JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
        $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
        JUCE_DONT_DECLARE_PROJECTINFO=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(dusk-studio-plugin-host PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_events
        juce::juce_core
        juce::juce_gui_basics
        juce::juce_graphics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    if(WIN32)
        target_link_libraries(dusk-studio-plugin-host PRIVATE synchronization)
    endif()
    target_include_directories(dusk-studio-plugin-host PRIVATE src)
    set_target_properties(dusk-studio-plugin-host PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:DuskStudio>")
    add_dependencies(DuskStudio dusk-studio-plugin-host)
endif()

target_include_directories(DuskStudio PRIVATE
    src
    external/projucer-shim)  # JuceHeader.h shim used by vendored plugin DSP headers

if(DEFINED DUSK_PLUGINS_PATH)
    # Per-plugin DSP headers from the Dusk plugins repo's main branch. Each
    # plugin owns its DSP; Dusk Studio vendors:
    #   - multi-q    (BritishEQProcessor — per-channel 4-band British EQ)
    #   - multi-comp (UniversalCompressor — per-channel Opto/FET/VCA comp,
    #                 used here as a juce::AudioProcessor; only its DSP path is
    #                 active — the plugin editor is compiled-in but never
    #                 instantiated by Dusk Studio's UI)
    #   - tape-echo    (TapeEchoDSP::TapeSaturation — legacy master bus tape sat,
    #                   being replaced by TapeMachine; still vendored while the
    #                   migration completes)
    #   - TapeMachine  (full-featured tape emulation hosted on the master bus;
    #                   editor opened via the gear button next to TAPE)
    # SYSTEM include — vendored DSP headers carry benign warnings
    # (shadows, sign conversions) we don't own. Marking them SYSTEM
    # tells GCC/Clang to suppress those warnings on inclusion, which
    # lets DUSKSTUDIO_STRICT_WARNINGS=-Werror apply to Dusk Studio's own sources
    # without the vendored headers blocking the build.
    target_include_directories(DuskStudio SYSTEM PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/shared"
        "${DUSK_PLUGINS_PATH}/plugins/shared/AnalogEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/shared/channel-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-q"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/HardwareEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/tape-echo/Source/DSP"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI")

    # Compile multi-comp's processor + editor + shared LEDMeter so
    # UniversalCompressor links cleanly. Editor sources are linked but not
    # invoked from Dusk Studio's UI — they exist only because UniversalCompressor's
    # createEditor() references them.
    target_sources(DuskStudio PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/shared/LEDMeter.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/ImprovedTapeEmulation.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/AnalogVUMeter.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/TapeReelComponent.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/TapeMachineLookAndFeel.cpp")

    # Both multi-comp's editor and TapeMachine's editor declare a class named
    # AnalogVUMeter in the global namespace, which collides at link time.
    # Rename the multi-comp one in-translation-unit so the donors stay
    # unmodified. Applied to every multi-comp .cpp that pulls in
    # AnalogLookAndFeel.h (directly or via EnhancedCompressorEditor.h).
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "AnalogVUMeter=MultiCompAnalogVUMeter")

    # Both donors define createPluginFilter() as their JUCE plugin entry
    # point. Dusk Studio instantiates them directly so neither is called, but the
    # global symbol still collides at link. Rename the TapeMachine one.
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "createPluginFilter=createTapeMachinePluginFilter")

    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_DUSK_DSP=1)
endif()

# Sfizz native sampler engine (BSD-2-Clause). Vendored as a git
# submodule under external/sfizz; built as a static library so we
# don't ship a separate .so/.dylib + dlopen at startup. Library-only
# build: no JACK CLI, no MIDI-file renderer, no benchmarks/tests/demos.
# DuskMultisampleProcessor (src/engine/multisample/) wraps it as a
# juce::AudioPluginInstance.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/src/sfizz.h")
    set(SFIZZ_JACK      OFF CACHE BOOL "" FORCE)
    set(SFIZZ_RENDER    OFF CACHE BOOL "" FORCE)
    set(SFIZZ_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(SFIZZ_TESTS     OFF CACHE BOOL "" FORCE)
    set(SFIZZ_DEMOS     OFF CACHE BOOL "" FORCE)
    set(SFIZZ_DEVTOOLS  OFF CACHE BOOL "" FORCE)
    set(SFIZZ_SHARED    OFF CACHE BOOL "" FORCE)
    set(SFIZZ_USE_SNDFILE OFF CACHE BOOL "" FORCE)
    # We don't ship sfizz as an LV2 / VST3 plugin - just the library.
    set(SFIZZ_LV2  OFF CACHE BOOL "" FORCE)
    set(SFIZZ_VST3 OFF CACHE BOOL "" FORCE)
    set(SFIZZ_VST2 OFF CACHE BOOL "" FORCE)
    set(SFIZZ_AU   OFF CACHE BOOL "" FORCE)
    set(SFIZZ_GIT_SUBMODULE_CHECK OFF CACHE BOOL "" FORCE)

    # Configure-time patches against vendored sfizz v1.2.3. Re-applied
    # idempotently on every reconfigure so a `git submodule update`
    # that restores the upstream files doesn't break the build.
    #
    #   1) SfizzConfig.cmake: -mfpu=neon + -mfloat-abi=hard are armv7-
    #      only flags but sfizz's regex matches "arm.*" which includes
    #      arm64 (Apple Silicon + aarch64 Linux). Clang on arm64
    #      rejects both flags.
    #   2) atomic_queue.h: `Base::template do_pop_any` /
    #      `do_push_any` reference non-template members. Apple Clang 21
    #      promotes the resulting -Wmissing-template-arg-list-after-
    #      template-kw warning to an error.
    set(_duskstudio_sfizz_cfg
        "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/cmake/SfizzConfig.cmake")
    file(READ "${_duskstudio_sfizz_cfg}" _duskstudio_sfizz_cfg_src)
    string(REPLACE "elseif(PROJECT_SYSTEM_PROCESSOR MATCHES \"(arm.*)\")"
                   "elseif(PROJECT_SYSTEM_PROCESSOR MATCHES \"(arm.*)\" AND NOT PROJECT_SYSTEM_PROCESSOR MATCHES \"(aarch64|arm64)\")"
                   _duskstudio_sfizz_cfg_patched "${_duskstudio_sfizz_cfg_src}")
    if(NOT _duskstudio_sfizz_cfg_src STREQUAL _duskstudio_sfizz_cfg_patched)
        file(WRITE "${_duskstudio_sfizz_cfg}" "${_duskstudio_sfizz_cfg_patched}")
        message(STATUS "Dusk Studio: patched sfizz SfizzConfig.cmake (skip armv7 flags on arm64)")
    endif()
    unset(_duskstudio_sfizz_cfg_src)
    unset(_duskstudio_sfizz_cfg_patched)
    unset(_duskstudio_sfizz_cfg)

    set(_duskstudio_atomic_queue_hdr
        "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/external/atomic_queue/include/atomic_queue/atomic_queue.h")
    file(READ "${_duskstudio_atomic_queue_hdr}" _duskstudio_aq_src)
    string(REPLACE "Base::template do_pop_any("  "Base::do_pop_any("  _duskstudio_aq_patched "${_duskstudio_aq_src}")
    string(REPLACE "Base::template do_push_any(" "Base::do_push_any(" _duskstudio_aq_patched "${_duskstudio_aq_patched}")
    if(NOT _duskstudio_aq_src STREQUAL _duskstudio_aq_patched)
        file(WRITE "${_duskstudio_atomic_queue_hdr}" "${_duskstudio_aq_patched}")
        message(STATUS "Dusk Studio: patched sfizz atomic_queue.h (drop spurious template keyword)")
    endif()
    unset(_duskstudio_aq_src)
    unset(_duskstudio_aq_patched)
    unset(_duskstudio_atomic_queue_hdr)

    #   3) Voice.cpp: `0x1.fffffep-1` C99 hex-float literal. GCC 15
    #      rejects hex-float literals in -std=c++14 mode (sfizz pins
    #      that standard), reporting "exponent has no digits". The
    #      replacement decimal rounds back to exactly the same float
    #      (the IEEE single just below 1.0).
    set(_duskstudio_voice_cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/src/sfizz/Voice.cpp")
    file(READ "${_duskstudio_voice_cpp}" _duskstudio_voice_src)
    string(REPLACE "0x1.fffffep-1" "0.99999994f"
                   _duskstudio_voice_patched "${_duskstudio_voice_src}")
    if(NOT _duskstudio_voice_src STREQUAL _duskstudio_voice_patched)
        file(WRITE "${_duskstudio_voice_cpp}" "${_duskstudio_voice_patched}")
        message(STATUS "Dusk Studio: patched sfizz Voice.cpp (replace C99 hex-float literal for GCC 15 -std=c++14)")
    endif()
    unset(_duskstudio_voice_src)
    unset(_duskstudio_voice_patched)
    unset(_duskstudio_voice_cpp)

    # Belt-and-suspenders integrity check: re-read after the patches
    # ran and abort configure if any anchor string is back to its
    # pre-patch form. Catches the case where a developer ran
    # `git submodule update` after configure (restoring upstream)
    # but didn't reconfigure - the build would otherwise fail later
    # with cryptic compile errors instead of a clear up-front message.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/cmake/SfizzConfig.cmake" _dsk_chk_a)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/external/atomic_queue/include/atomic_queue/atomic_queue.h" _dsk_chk_b)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/external/sfizz/src/sfizz/Voice.cpp" _dsk_chk_c)
    if(_dsk_chk_a MATCHES "elseif\\(PROJECT_SYSTEM_PROCESSOR MATCHES \"\\(arm\\.\\*\\)\"\\)\n[ \t]*add_compile_options\\(-mfpu=neon\\)"
       OR _dsk_chk_b MATCHES "Base::template do_pop_any"
       OR _dsk_chk_c MATCHES "0x1\\.fffffep-1")
        message(FATAL_ERROR
            "external/sfizz patches missing - the submodule was likely "
            "restored by `git submodule update` after configure last ran. "
            "Re-run cmake configure (the patches re-apply automatically) "
            "before building.")
    endif()
    unset(_dsk_chk_a)
    unset(_dsk_chk_b)
    unset(_dsk_chk_c)

    add_subdirectory(external/sfizz EXCLUDE_FROM_ALL)
    target_link_libraries(DuskStudio PRIVATE sfizz::static)
    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_MULTISAMPLE=1)
    target_sources(DuskStudio PRIVATE
        src/engine/multisample/DuskMultisampleProcessor.cpp
        src/engine/multisample/DuskMultisamplePluginFormat.cpp
        src/engine/multisample/AriaBank.cpp
        src/engine/multisample/AriaGui.cpp
        src/engine/multisample/Sf2Reader.cpp
        src/engine/multisample/Sf2ToSfz.cpp
        src/ui/multisample/DuskMultisampleEditor.cpp
        src/ui/multisample/AriaGuiComponent.cpp)

    # SF2 (SoundFont 2) playback is native: Sf2Reader + Sf2ToSfz convert
    # the SoundFont to SFZ + extracted WAVs and run it through the
    # already-vendored sfizz engine. No fluidsynth dependency, so a
    # self-contained install plays SF2 with nothing extra to bundle.
endif()

target_compile_definitions(DuskStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_WAYLAND}>:DUSKSTUDIO_JUCE_HAS_WAYLAND=1>
    JUCE_APPLICATION_NAME_STRING="Dusk Studio"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DuskStudio,JUCE_VERSION>"
    JUCE_JACK=1
    JUCE_ALSA=1
    # Native Wayland windowing on Linux (plugdata fork). Bypasses XWayland
    # and the meta_window_unmanage assertion that crashes mutter when an
    # X11 toplevel is destroyed while focused.
    $<$<PLATFORM_ID:Linux>:JUCE_WAYLAND=1>
    # Plugin host formats. Default JUCE setting is OFF; turn on the two
    # formats we actually scan + load (VST3 and LV2 - both popular on Linux
    # via yabridge / native ports). Without these, addDefaultFormats()
    # registers nothing and every plugin load fails with "No compatible
    # plug-in format exists for this plug-in".
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    # AU is the dominant native plugin format on macOS; without this
    # define the format is silently absent from addDefaultFormats().
    JUCE_PLUGINHOST_AU=$<IF:$<PLATFORM_ID:Darwin>,1,0>
    # Projucer-style macros that vendored plugin sources expect. multi-comp
    # uses VersionString / Name / Manufacturer; TapeMachine additionally
    # references the IsMidiEffect / IsSynth / WantsMidiInput / ProducesMidiOutput
    # toggles from inside #if blocks, so we provide defaults (all 0) globally
    # rather than per-source.
    JucePlugin_VersionString="${PROJECT_VERSION}"
    JucePlugin_Name="DuskStudio_Comp"
    JucePlugin_Manufacturer="Marc Korte"
    JucePlugin_IsMidiEffect=0
    JucePlugin_IsSynth=0
    JucePlugin_WantsMidiInput=0
    JucePlugin_ProducesMidiOutput=0)

if((CMAKE_SYSTEM_NAME STREQUAL "Linux") AND TARGET juce::pkgconfig_JUCE_WAYLAND_LINUX_DEPS)
    # Pulls in wayland-client / xkbcommon / libdecor-0 include paths.
    # Linkage is dlopen at runtime (NOLINK in the JUCE pkg-config target),
    # so this is purely for the headers exposed via juce_gui_basics.h
    # under JUCE_WAYLAND.
    target_link_libraries(DuskStudio PRIVATE juce::pkgconfig_JUCE_WAYLAND_LINUX_DEPS)
endif()

target_link_libraries(DuskStudio PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# LTO is on by default for release builds (5-10% smaller binary, slight perf
# win) but it serializes the GCC link stage — lto-wrapper runs LTRANS jobs
# one at a time, idling 127 of your 128 cores for 2-4 minutes per build.
# For local dev iterations that's painful, so make it opt-in: pass
# -DDUSKSTUDIO_ENABLE_LTO=ON when you want it (CI / release packaging).
if(DUSKSTUDIO_ENABLE_LTO)
    target_link_libraries(DuskStudio PRIVATE juce::juce_recommended_lto_flags)
else()
    set_property(TARGET DuskStudio PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Treat warnings as errors on the Dusk Studio target. Off by default; opt-in
# via -DDUSKSTUDIO_STRICT_WARNINGS=ON. Not yet enabled in CI because the
# vendored multi-comp .cpp sources compiled into Dusk Studio still emit
# shadow / sign-conversion warnings (vendored DSP HEADERS are marked
# SYSTEM above and suppressed; the COMPILED .cpp from the plugins repo
# is not). Flip the CI workflow once those are cleaned upstream or
# wrapped with per-source warning overrides via set_source_files_properties.
option(DUSKSTUDIO_STRICT_WARNINGS "Treat warnings as errors on the Dusk Studio target" OFF)
if(DUSKSTUDIO_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(DuskStudio PRIVATE /WX)
    else()
        target_compile_options(DuskStudio PRIVATE -Werror)
    endif()
endif()

# AddressSanitizer + UBSan. Off by default; opt-in via
# -DDUSKSTUDIO_ENABLE_ASAN=ON. Applied build-wide (directory-level options)
# because sanitizer-instrumented code can't link against
# non-instrumented code without symbol-resolution drama. CI runs the
# test binary under ASAN to catch heap corruption / use-after-free /
# undefined-behaviour from a long-running session's worth of edits.
# Not for production builds — overhead is ~2× CPU and ~3× memory.
option(DUSKSTUDIO_ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)

# ThreadSanitizer. Off by default; opt-in via -DDUSKSTUDIO_ENABLE_TSAN=ON.
# Verifies the lock-free atomic / SPSC FIFO / message-thread-only-mutation
# invariants the engine relies on (see CLAUDE.md "Audio thread rules").
# CI runs the test binary under TSan in a separate job from ASan because:
#   • TSan and ASan use the same shadow-memory machinery and refuse to
#     co-exist in a single binary (the runtime aborts immediately).
#   • TSan's overhead (~5–15× CPU, ~5–10× memory) makes a combined run
#     untenable on a 2-core GitHub runner.
# Same build-wide application as ASan: instrumented code can't link
# against uninstrumented dependencies cleanly.
option(DUSKSTUDIO_ENABLE_TSAN "Enable ThreadSanitizer (TSan)" OFF)

if(DUSKSTUDIO_ENABLE_TSAN AND DUSKSTUDIO_ENABLE_ASAN)
    # Compiler-runtime constraint, not a Dusk Studio choice — the
    # instrumentation libraries (libasan / libtsan) can't share a
    # process. Fail at configure time so the user sees the cause
    # rather than a runtime abort after a 5-minute build.
    message(FATAL_ERROR
        "DUSKSTUDIO_ENABLE_TSAN and DUSKSTUDIO_ENABLE_ASAN are mutually "
        "exclusive — ThreadSanitizer and AddressSanitizer cannot share "
        "the same binary. Pick one for this configure, and run a "
        "separate build directory for the other.")
endif()

if(DUSKSTUDIO_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(DUSKSTUDIO_ENABLE_TSAN AND NOT MSVC)
    # -g forces full DWARF for inlined frames so TSan stack traces resolve
    # to file:line. RelWithDebInfo would carry -g already; Debug + this
    # combo is robust across both build types.
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

option(DUSKSTUDIO_BUILD_TESTS "Build Dusk Studio unit tests with Catch2" OFF)
if(DUSKSTUDIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Packaging via CPack. Generators are selected by scripts/package-*.{sh,ps1}
# at cpack invocation time (-G DEB / -G RPM / -G WIX / -G DragNDrop);
# this block just supplies the metadata each generator reads. The
# AppImage path is NOT a CPack generator - linuxdeploy + appimagetool
# stage AppDir from the same install() rules below.
install(TARGETS DuskStudio
        RUNTIME DESTINATION bin
        BUNDLE  DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/audio.dusk.studio.desktop"
        DESTINATION share/applications)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/DuskStudio.appdata.xml"
        DESTINATION share/metainfo)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/DuskStudio.mime.xml"
        DESTINATION share/mime/packages)
# Packaging icon: hard requirement only when DUSKSTUDIO_PACKAGE_BUILD=ON
# (release packaging path). The icon is a brand asset not committed to
# the repo, so dev builds and CI tests stay buildable without it.
# scripts/package-appimage.sh sets the option (and would fail at runtime
# if the icon were missing anyway). install() is gated on file presence
# so a non-packaging dev build doesn't include a stale install rule.
option(DUSKSTUDIO_PACKAGE_BUILD
       "Enforce assets required for distribution packaging (icon, etc.)" OFF)
if(DUSKSTUDIO_PACKAGE_BUILD AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/DuskStudio.png")
    message(FATAL_ERROR
        "packaging/DuskStudio.png is required for packaging. "
        "Provide a 256x256 PNG at ${CMAKE_CURRENT_SOURCE_DIR}/packaging/DuskStudio.png "
        "(brand asset, not committed to the repo).")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/DuskStudio.png")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/DuskStudio.png"
            DESTINATION share/icons/hicolor/256x256/apps)
endif()

set(CPACK_PACKAGE_NAME              "dusk-studio")
set(CPACK_PACKAGE_VENDOR            "Dusk Audio")
set(CPACK_PACKAGE_VERSION           "${DUSKSTUDIO_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Portastudio-style 24-channel DAW")
set(CPACK_PACKAGE_HOMEPAGE_URL      "https://github.com/dusk-audio/dusk-studio")
set(CPACK_PACKAGE_CONTACT           "Dusk Audio <support@duskaudio.example>")
set(CPACK_RESOURCE_FILE_LICENSE     "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Dusk Studio")

# Debian: runtime deps gathered automatically via dpkg-shlibdeps when
# the package is built on an Ubuntu/Debian host. JUCE statically links
# audio/MIDI; the audible runtime needs are PipeWire (libjack-replacement)
# and ALSA fallback. GTK is pulled in by juce_gui_basics.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS  ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS    "libc6, libstdc++6, libasound2, libgtk-3-0, libxcb1, libxcb-cursor0")
set(CPACK_DEBIAN_PACKAGE_SECTION    "sound")
set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")

# RPM: dependency auto-extraction from ELF symbols; explicit hints for
# the dynamic GTK/X11 stack that JUCE expects on Fedora/RHEL.
set(CPACK_RPM_PACKAGE_REQUIRES      "alsa-lib, gtk3, libxcb, xcb-util-cursor")
set(CPACK_RPM_PACKAGE_LICENSE       "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP         "Applications/Multimedia")

# Windows WIX MSI. GUIDs are STABLE per product family; the upgrade
# GUID lets newer installers replace older ones in-place.
set(CPACK_WIX_UPGRADE_GUID          "8F2C9C3E-5F4B-4D9E-B7A1-3E7E8D9F0C12")
set(CPACK_WIX_PRODUCT_GUID          "*")  # generated per build per WIX best practice

# The WIX generator only accepts a .txt or .rtf license and derives the
# type from the extension; the repo's LICENSE is extensionless (fine for
# deb/rpm/dmg) so cpack -G WIX dies with "unsupported WiX License file
# extension ''". Stage a .txt copy and point the license at it on Windows
# only — cpack converts the .txt to RTF for the installer's EULA page.
if(WIN32)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                   "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")

    # Start Menu + desktop shortcuts. Without CPACK_PACKAGE_EXECUTABLES the
    # WIX generator installs the .exe to bin/ but creates no shortcut, so the
    # app only appears in the install folder. The pair is
    # "<exe-name-without-.exe>" "<shortcut label>"; CREATE_DESKTOP_LINKS adds
    # the desktop icon for the same executable.
    set(CPACK_PACKAGE_EXECUTABLES "DuskStudio" "Dusk Studio")
    set(CPACK_CREATE_DESKTOP_LINKS "DuskStudio")
endif()

# macOS DMG: cpack -G DragNDrop. scripts/package-macos.sh wraps it with
# an ad-hoc codesign (unsigned by design - no notarization).
set(CPACK_DMG_FORMAT                "UDZO")
set(CPACK_DMG_VOLUME_NAME           "Dusk Studio ${DUSKSTUDIO_VERSION}")

# Package filename platform suffix. CPack defaults to the kernel name
# ("Darwin") on macOS and "win64" on Windows - override both to a user-facing,
# OS-and-arch-explicit suffix so every platform shares one scheme:
# dusk-studio-<version>-<OS>-<arch>.<ext> (macOS-arm64 / Windows-x64 /
# Linux-x86_64). macOS is Apple-Silicon-only (won't run on Intel Macs).
if(APPLE)
    set(CPACK_SYSTEM_NAME "macOS-arm64")
elseif(WIN32)
    set(CPACK_SYSTEM_NAME "Windows-x64")
endif()

include(CPack)
