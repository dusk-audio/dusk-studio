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

project(DuskStudio VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JUCE path resolution. Per-OS defaults so a developer toggling between
# Mac (upstream JUCE) and Linux (plugdata-team JUCE-wayland fork, required
# for the wayland-juce8 features) doesn't have to pass -DJUCE_PATH each time.
# Priority:
#   1. -DJUCE_PATH (explicit override)
#   2. Linux: sibling ../JUCE-wayland (wayland fork)
#   3. Any OS: sibling ../JUCE (upstream)
#   4. external/JUCE submodule
# The wayland fork has its own XEmbed + addDefaultFormatsToManager API that
# Focal's source assumes on Linux; see [src/engine/JuceCompat.h](src/engine/JuceCompat.h)
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
# Skippable via -DFOCAL_SKIP_FORK_CHECK=ON for CI / test builds that use a
# vanilla upstream JUCE clone (no plugin editors involved, so the XEmbed patch
# is irrelevant). Never set this for a distribution build — pure-xcb VST3
# plugins will render blank without the patch.
option(DUSKSTUDIO_SKIP_FORK_CHECK
       "Skip the JUCE-wayland XEmbed patch enforcement (CI / tests only)" OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT DUSKSTUDIO_SKIP_FORK_CHECK)
    set(_focal_xembed_file
        "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/juce_gui_extra/native/juce_XEmbedComponent_linux.cpp")
    if(NOT EXISTS "${_focal_xembed_file}")
        message(FATAL_ERROR "Expected JUCE source not found: ${_focal_xembed_file}")
    endif()
    file(READ "${_focal_xembed_file}" _focal_xembed_src)
    if(NOT _focal_xembed_src MATCHES "enum class Role { host, client }")
        message(FATAL_ERROR
            "Dusk Studio requires a patched JUCE: the XEmbed host-mapping fix is missing in\n"
            "  ${_focal_xembed_file}\n"
            "Without it, pure-xcb VST3 plugins (e.g. MininnDrum) render as a blank\n"
            "rectangle. Re-apply the local commit titled\n"
            "  \"XEmbed: stop tying host-window mapping to client _XEMBED_INFO\"\n"
            "in your JUCE checkout, or pull a JUCE branch that contains it.\n"
            "(CI / test builds: pass -DFOCAL_SKIP_FORK_CHECK=ON to bypass.)")
    endif()
    unset(_focal_xembed_file)
    unset(_focal_xembed_src)
endif()

# Detect plugdata-team JUCE-wayland's free juce::addDefaultFormatsToManager
# function at configure time. The fork added it + marked the member
# = delete; upstream JUCE only has the member. JuceCompat.h picks the
# right API via the DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS preprocessor define.
# Done at configure time because SFINAE on the free function fails:
# the qualified name lookup happens at template definition, not
# substitution, so calling code wouldn't compile against upstream.
set(DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS OFF)
set(_focal_apf_helpers
    "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/juce_audio_processors/format/juce_AudioPluginFormatManagerHelpers.h")
if(EXISTS "${_focal_apf_helpers}")
    file(READ "${_focal_apf_helpers}" _focal_apf_src)
    if(_focal_apf_src MATCHES "addDefaultFormatsToManager")
        set(DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS ON)
        message(STATUS "JUCE has free juce::addDefaultFormatsToManager (wayland fork)")
    endif()
    unset(_focal_apf_src)
endif()
unset(_focal_apf_helpers)

# Detect the wayland fork's WaylandWindowSystem / WaylandSymbols. The
# fork integrates libwayland-client + libdecor directly for top-level
# windowing; upstream JUCE has no such symbols. Focal's
# PlatformWindowing_Linux.cpp uses them to drive the X11-vs-Wayland
# peer-creation latch + the requestFocusOnMainWaylandSurface roundtrip
# yield. CI builds against upstream JUCE (no Wayland symbols) need
# those code paths stubbed.
set(DUSKSTUDIO_JUCE_HAS_WAYLAND OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_focal_juce_wayland_dir "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/juce_gui_basics/native")
    file(GLOB _focal_juce_wayland_files "${_focal_juce_wayland_dir}/*.h")
    foreach(_f ${_focal_juce_wayland_files})
        file(READ "${_f}" _focal_juce_wayland_src)
        if(_focal_juce_wayland_src MATCHES "class +WaylandWindowSystem")
            set(DUSKSTUDIO_JUCE_HAS_WAYLAND ON)
            message(STATUS "JUCE has WaylandWindowSystem (wayland fork): ${_f}")
            break()
        endif()
    endforeach()
    unset(_focal_juce_wayland_files)
    unset(_focal_juce_wayland_src)
    unset(_focal_juce_wayland_dir)
endif()

add_subdirectory(${DUSKSTUDIO_JUCE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

# App icon. JUCE bakes the PNG into platform-specific resources at
# build time (.ico on Windows, .icns on macOS, embedded data block on
# Linux). Same file is used for both BIG and SMALL - JUCE downscales
# internally as needed. The conditional keeps a fresh checkout
# building when assets/icon.png hasn't been added yet.
set(DUSKSTUDIO_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png")
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
    PRODUCT_NAME       "Dusk Studio"
    COMPANY_NAME       "Marc Korte"
    BUNDLE_ID          "com.marckorte.focal"
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

# macOS code signing. Ad-hoc by default (works for local dev launch +
# satisfies the runtime-loadable plugin requirement that the host be
# at least ad-hoc signed). For distribution: set
#   -DFOCAL_CODESIGN_IDENTITY="Developer ID Application: <name> (TEAMID)"
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
                          --entitlements "${CMAKE_CURRENT_BINARY_DIR}/DuskStudio_artefacts/JuceLibraryCode/Dusk Studio.entitlements"
                          --sign "${DUSKSTUDIO_CODESIGN_IDENTITY}"
                          "$<TARGET_BUNDLE_DIR:DuskStudio>"
        COMMENT "patch Info.plist + codesign "Dusk Studio.app" (identity='${DUSKSTUDIO_CODESIGN_IDENTITY}')"
        VERBATIM)
endif()

# Dusk Audio plugins repo — donor for the analog DSP cores (4K EQ filter chain,
# FET/Opto compressor, Pultec EQ, tape saturation, console saturation, plus the
# AnalogEmulation library). Header-only cores are picked up via the include
# paths below; editing a header in the plugins repo automatically rebuilds
# the affected Focal translation units. No copy / submodule bump required.
#
# Resolution priority:
#   1. -DDUSK_PLUGINS_PATH (explicit override)
#   2. sibling ../plugins-main — a permanent worktree pinned to plugins-`main`.
#      Use this when the working ../plugins checkout is on a feature branch
#      that may not have the donor API Focal expects. Create with:
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
    src/engine/BounceEngine.cpp
    src/engine/FileImporter.cpp
    src/engine/MasteringPlayer.cpp
    src/engine/MidiClockEmitter.cpp
    src/engine/MidiSyncReceiver.cpp
    src/engine/MidiTimeCodeReceiver.cpp
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
    src/ui/AuxEditorHost.cpp
    src/ui/AuxLaneComponent.cpp
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
    src/ui/MultiImportTargetPicker.cpp
    src/ui/MasteringEqEditor.cpp
    src/ui/MasteringLimiterEditor.cpp
    src/ui/MasteringView.cpp
    src/ui/MasterStripComponent.cpp
    src/ui/MidiBindingsPanel.cpp
    src/ui/PianoRollComponent.cpp
    src/ui/PluginPickerHelpers.cpp
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
# driver class; the audio settings panel then shows ASIO devices alongside
# WASAPI / DirectSound. Missing SDK is non-fatal - the build falls back to
# the JUCE-default WASAPI/DirectSound backends and prints a status note.
if(WIN32)
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
    else()
        message(STATUS "Dusk Studio: ASIO SDK not found (set -DASIOSDK_PATH=... or place at ../asiosdk to enable). "
                       "Falling back to WASAPI / DirectSound.")
    endif()
endif()

# Focal-owned ALSA backend - a juce::AudioIODevice + AudioIODeviceType pair
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
    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_FOCAL_ALSA=1)

    # Out-of-process plugin hosting: shared IPC primitives + the
    # parent-side connection class + a self-test driver. The
    # dusk-studio-plugin-host child binary lives next to Focal in the build
    # output. Linux-only; the IPC uses memfd_create + futex which are
    # Linux-specific. Mac/Windows builds keep using the in-process path
    # (PluginSlot) until the OOP work matures into a portable design.
    # DUSKSTUDIO_HAS_OOP_PLUGINS gates every #if branch in PluginSlot that
    # routes through RemotePluginConnection; defined only here so the
    # rest of the codebase can rely on a single feature flag.
    target_sources(DuskStudio PRIVATE
        src/engine/ipc/RemotePluginConnection.cpp
        src/engine/ipc/IpcSelfTest.cpp)
    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_OOP_PLUGINS=1)

    # dusk-studio-plugin-host is a JUCE console app so it can pull in the
    # AudioPluginFormatManager + AudioPluginInstance machinery and
    # (in Phase 3) the gui_extra modules for plugin-editor embedding.
    # juce_add_console_app gives us juce::JUCEApplicationBase plumbing
    # without forcing a UI; we still set NEEDS_*_GRAPHICS / GUI for the
    # editor module to compile. The host's main() runs JUCE's message
    # loop on the main thread + a worker juce::Thread for the audio path.
    juce_add_console_app(dusk-studio-plugin-host
        PRODUCT_NAME "dusk-studio-plugin-host"
        COMPANY_NAME "Marc Korte")
    target_sources(dusk-studio-plugin-host PRIVATE
        src/engine/ipc/PluginHostMain.cpp)
    target_compile_features (dusk-studio-plugin-host PRIVATE cxx_std_17)
    target_compile_definitions (dusk-studio-plugin-host PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LV2=1
        JUCE_PLUGINHOST_AU=0
        $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
        # Stop juce_audio_processors from generating JuceHeader.h-style
        # stubs and Projucer-only metadata - we drive everything from
        # CMake.
        JUCE_DONT_DECLARE_PROJECTINFO=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)
    target_link_libraries(dusk-studio-plugin-host PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_events
        juce::juce_core
        # Phase 4 editor embedding: createEditorIfNeeded returns a
        # juce::AudioProcessorEditor (which derives from Component);
        # hosting it in a borderless DocumentWindow to expose its native
        # X11 Window ID over IPC needs gui_basics + graphics. Linked
        # explicitly rather than relying on transitive resolution from
        # audio_processors so an upstream JUCE refactor doesn't silently
        # drop them.
        juce::juce_gui_basics
        juce::juce_graphics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
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
    # plugin owns its DSP; Focal vendors:
    #   - multi-q    (BritishEQProcessor — per-channel 4-band British EQ)
    #   - multi-comp (UniversalCompressor — per-channel Opto/FET/VCA comp,
    #                 used here as a juce::AudioProcessor; only its DSP path is
    #                 active — the plugin editor is compiled-in but never
    #                 instantiated by Focal's UI)
    #   - tape-echo    (TapeEchoDSP::TapeSaturation — legacy master bus tape sat,
    #                   being replaced by TapeMachine; still vendored while the
    #                   migration completes)
    #   - TapeMachine  (full-featured tape emulation hosted on the master bus;
    #                   editor opened via the gear button next to TAPE)
    # SYSTEM include — vendored DSP headers carry benign warnings
    # (shadows, sign conversions) we don't own. Marking them SYSTEM
    # tells GCC/Clang to suppress those warnings on inclusion, which
    # lets DUSKSTUDIO_STRICT_WARNINGS=-Werror apply to Focal's own sources
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
    # invoked from Focal's UI — they exist only because UniversalCompressor's
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
    # point. Focal instantiates them directly so neither is called, but the
    # global symbol still collides at link. Rename the TapeMachine one.
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "createPluginFilter=createTapeMachinePluginFilter")

    target_compile_definitions(DuskStudio PRIVATE DUSKSTUDIO_HAS_DUSK_DSP=1)
endif()

target_compile_definitions(DuskStudio PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_WAYLAND}>:DUSKSTUDIO_JUCE_HAS_WAYLAND=1>
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DuskStudio,JUCE_PRODUCT_NAME>"
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
    JucePlugin_Name="Focal_Comp"
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
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Treat warnings as errors on the Focal target. Off by default; opt-in
# via -DFOCAL_STRICT_WARNINGS=ON. Not yet enabled in CI because the
# vendored multi-comp .cpp sources compiled into Focal still emit
# shadow / sign-conversion warnings (vendored DSP HEADERS are marked
# SYSTEM above and suppressed; the COMPILED .cpp from the plugins repo
# is not). Flip the CI workflow once those are cleaned upstream or
# wrapped with per-source warning overrides via set_source_files_properties.
option(DUSKSTUDIO_STRICT_WARNINGS "Treat warnings as errors on the Focal target" OFF)
if(DUSKSTUDIO_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(DuskStudio PRIVATE /WX)
    else()
        target_compile_options(DuskStudio PRIVATE -Werror)
    endif()
endif()

# AddressSanitizer + UBSan. Off by default; opt-in via
# -DFOCAL_ENABLE_ASAN=ON. Applied build-wide (directory-level options)
# because sanitizer-instrumented code can't link against
# non-instrumented code without symbol-resolution drama. CI runs the
# test binary under ASAN to catch heap corruption / use-after-free /
# undefined-behaviour from a long-running session's worth of edits.
# Not for production builds — overhead is ~2× CPU and ~3× memory.
option(DUSKSTUDIO_ENABLE_ASAN "Enable AddressSanitizer + UBSan" OFF)
if(DUSKSTUDIO_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

option(DUSKSTUDIO_BUILD_TESTS "Build Focal unit tests with Catch2" OFF)
if(DUSKSTUDIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
