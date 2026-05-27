include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(dusk-studio-tests
    smoke_brickwall_limiter.cpp
    session_tick_conversion.cpp
    atomic_park.cpp
    embedded_modal_close_safety.cpp
    x11_anti_pattern_guard.cpp
    snap_helpers.cpp
    fade_shapes.cpp
    crossfade.cpp
    fade_curve_path.cpp
    file_importer_audio.cpp
    hardware_insert.cpp
    session_round_trip.cpp
    session_corrupted_recovery.cpp
    session_format_version.cpp
    recording_byte_accuracy.cpp
    midi_fifo_overflow.cpp
    midi_time_code_decode.cpp
    midi_time_code_emit.cpp
    automation_thinning.cpp
    session_midi_tempolock_roundtrip.cpp
    session_apply_tempo_change.cpp
    transport_state_machine.cpp
    session_mcu_roundtrip.cpp
    mcu_receiver_decode.cpp
    mcu_controller_emit.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/McuReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/McuController.cpp
    multisample_state_roundtrip.cpp
    bounce_stem_filename.cpp
    metronome_click.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/Metronome.cpp
    aria_gui_parse.cpp
    sf2_reader_parse.cpp
    sf2_to_sfz_convert.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/DuskMultisampleProcessor.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/AriaBank.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/AriaGui.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/Sf2Reader.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/Sf2ToSfz.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/multisample/DuskMultisampleEditor.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/multisample/AriaGuiComponent.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/DuskFileBrowser.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/DimOverlay.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/BrickwallLimiter.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/HardwareInsertSlot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FileImporter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/RecordManager.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiTimeCodeReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiTimeCodeEmitter.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Session.cpp
    ${CMAKE_SOURCE_DIR}/src/session/SessionSerializer.cpp)

target_include_directories(dusk-studio-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/projucer-shim)

target_compile_definitions(dusk-studio-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
    DUSKSTUDIO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(dusk-studio-tests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_audio_basics
    # juce_audio_devices required by SessionSerializer.cpp for
    # juce::MidiInput::getAvailableDevices() — resolves Track::
    # midiInputIdentifier back to an index on session load. Removing
    # this triggers an ld undefined-reference on the MidiInput /
    # MidiOutput statics.
    juce::juce_audio_devices
    juce::juce_audio_formats
    # juce_audio_processors brings in juce::AudioPluginInstance for
    # multisample tests (DuskMultisampleProcessor subclasses it).
    juce::juce_audio_processors
    # juce_gui_basics required by DuskMultisampleEditor (compiled into
    # the test binary via the multisample tests). The tests don't
    # construct the editor; we just link the symbol it references
    # through createEditor().
    juce::juce_gui_basics
    juce::juce_core
    juce::juce_dsp
    juce::juce_graphics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Multisample tests need sfizz_static + the DUSKSTUDIO_HAS_MULTISAMPLE
# define for the same gating the main app uses. Skip when sfizz isn't
# vendored (the multisample sources are then a no-op too).
if(TARGET sfizz_static)
    target_link_libraries(dusk-studio-tests PRIVATE sfizz::static)
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_MULTISAMPLE=1)
endif()

# IPC round-trip regression — Linux + Windows + macOS 14.4+. The test
# spawns the dusk-studio-plugin-host child in --ipc-stub mode, so we
# hard-depend on that target being built first. DUSKSTUDIO_PLUGIN_HOST_PATH
# is resolved via a generator expression so it points to wherever CMake
# actually puts the binary in the current build configuration.
set(_duskstudio_ipc_test_enabled FALSE)
if (UNIX AND NOT APPLE)
    set(_duskstudio_ipc_test_enabled TRUE)
    set(_duskstudio_ipc_test_sources
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcChannel_Linux.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcProcess_Linux.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcShm_Linux.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcSync_Linux.cpp)
elseif (WIN32)
    set(_duskstudio_ipc_test_enabled TRUE)
    set(_duskstudio_ipc_test_sources
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcChannel_Windows.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcProcess_Windows.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcShm_Windows.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcSync_Windows.cpp)
elseif (APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER_EQUAL 14.4)
    set(_duskstudio_ipc_test_enabled TRUE)
    set(_duskstudio_ipc_test_sources
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcChannel_Mac.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcProcess_Mac.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcShm_Mac.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/platform/IpcSync_Mac.cpp)
endif()
if (_duskstudio_ipc_test_enabled)
    target_sources(dusk-studio-tests PRIVATE
        ipc_stub_round_trip.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/RemotePluginConnection.cpp
        ${_duskstudio_ipc_test_sources})
    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_PLUGIN_HOST_PATH="$<TARGET_FILE:dusk-studio-plugin-host>")
    if (WIN32)
        target_link_libraries(dusk-studio-tests PRIVATE synchronization)
    endif()
    add_dependencies(dusk-studio-tests dusk-studio-plugin-host)
endif()

# UniversalCompressor (multi-comp donor) tests — gated on the donor being
# available. Mirrors the main app's DUSK_PLUGINS_PATH wiring exactly so the
# tests link against the same DSP code paths the channel strip uses.
if(DEFINED DUSK_PLUGINS_PATH)
    target_sources(dusk-studio-tests PRIVATE
        comp_static_curve.cpp
        comp_timing.cpp
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/shared/LEDMeter.cpp")

    # Same AnalogVUMeter rename the main app applies — keeps the donor
    # untouched while preventing a global-namespace collision when other
    # donors with the same class name get linked in.
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "AnalogVUMeter=MultiCompAnalogVUMeter")

    target_include_directories(dusk-studio-tests SYSTEM PRIVATE
        "${DUSK_PLUGINS_PATH}/plugins/shared"
        "${DUSK_PLUGINS_PATH}/plugins/shared/AnalogEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/shared/channel-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-q"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/HardwareEmulation")

    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_HAS_DUSK_DSP=1
        # Projucer-style macros the donor sources reference unguarded.
        # Mirror the main app's defaults (CMakeLists.txt:710) — the values
        # are not load-bearing for tests; they just need to exist.
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JucePlugin_Name="DuskStudio_Comp_Tests"
        JucePlugin_Manufacturer="Marc Korte"
        JucePlugin_IsMidiEffect=0
        JucePlugin_IsSynth=0
        JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(dusk-studio-tests)
