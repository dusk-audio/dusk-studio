include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(dusk-studio-tests
    # Shared native-host foundation (PortLayout / PortBuffers / INativeInstance)
    # is header-only + platform-neutral, so it links in the base target.
    hosting_port_layout.cpp
    hosting_insert_adapter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/hosting/InsertAdapter.cpp
    # Native CLAP tests + sources are Linux-only (dlopen/poll) — added conditionally below.
    plugin_scan_protocol.cpp
    plugin_backing_check.cpp
    session_tick_conversion.cpp
    tempo_map_conversion.cpp
    output_pair_routing.cpp
    multiband_comp_presets.cpp
    atomic_park.cpp
    audio_worker_pool.cpp
    rt_priority.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/AudioWorkerPool.cpp
    embedded_modal_close_safety.cpp
    x11_anti_pattern_guard.cpp
    snap_helpers.cpp
    fade_shapes.cpp
    crossfade.cpp
    fade_curve_path.cpp
    file_importer_audio.cpp
    hardware_insert.cpp
    session_round_trip.cpp
    session_freeze_roundtrip.cpp
    session_region_bounds.cpp
    session_transport_bounds.cpp
    session_corrupted_recovery.cpp
    session_clamp_corrupt_values.cpp
    session_format_version.cpp
    session_write_atomic.cpp
    session_portable_paths.cpp
    session_track_shrink.cpp
    device_fallback_message.cpp
    device_interfaces.cpp
    update_checker_version.cpp
    session_schema_migration.cpp
    dsp_components_test.cpp
    chord_analyzer.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/ChordAnalyzer.cpp
    pitch_detector.cpp
    recording_byte_accuracy.cpp
    midi_fifo_overflow.cpp
    midi_device_layer.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/midi/MidiDevices.cpp
    midi_time_code_decode.cpp
    midi_time_code_emit.cpp
    midi_clock_drift.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiClockEmitter.cpp
    smpte_wrap.cpp
    automation_thinning.cpp
    automation_lane_snapshot.cpp
    pdc_math.cpp
    session_midi_tempolock_roundtrip.cpp
    session_apply_tempo_change.cpp
    session_fader_group.cpp
    transport_state_machine.cpp
    session_mcu_roundtrip.cpp
    mcu_receiver_decode.cpp
    mcu_controller_emit.cpp
    mcu_fader_taper.cpp
    foundation_message_thread.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/McuReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/McuController.cpp
    ${CMAKE_SOURCE_DIR}/src/foundation/MessageThread.cpp
    multisample_state_roundtrip.cpp
    bounce_stem_filename.cpp
    bounce_stem_targets.cpp
    dp_importer_parse.cpp
    dp_aligner_align.cpp
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
    # DuskComboBox.cpp carries the vtable for duskstudio::DuskComboBox, which the
    # multisample editor now uses (its showPopup override) — link it so the
    # editor TU resolves.
    ${CMAKE_SOURCE_DIR}/src/ui/DuskComboBox.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/HardwareInsertSlot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FileImporter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DpImporter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DpAligner.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/RecordManager.cpp
    record_midi_overdub_diff.cpp
    playback_loop_read.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PlaybackEngine.cpp
    mastering_player_resample.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MasteringPlayer.cpp
    session_missing_sections.cpp
    session_save_as_consolidation.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiTimeCodeReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiTimeCodeEmitter.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Session.cpp
    ${CMAKE_SOURCE_DIR}/src/session/SessionSerializer.cpp)

target_include_directories(dusk-studio-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/projucer-shim
    ${CMAKE_SOURCE_DIR}/external/clap/include)
target_include_directories(dusk-studio-tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/external/json)

# JUCE-free foundation helpers (dusk::text / dusk::fs / dusk::json) — no external deps.
target_sources(dusk-studio-tests PRIVATE
    foundation_text.cpp
    foundation_text_format.cpp
    foundation_fs.cpp
    foundation_special_locations.cpp
    foundation_json.cpp
    foundation_audio_math.cpp
    foundation_int_delay_line.cpp
    foundation_midi_buffer.cpp
    midi_ring.cpp
    midi_collector.cpp
    lv2_state_paths.cpp)

# Phase 1a de-JUCE: JUCE-free audio file I/O (libsndfile backend), test-only for
# now. Optional — where libsndfile isn't installed (some CI platforms) the unit
# and its tests are skipped rather than failing configure; it's Linux-first.
find_library(SNDFILE_LIBRARY NAMES sndfile)
find_path(SNDFILE_INCLUDE_DIR sndfile.h)
if(SNDFILE_LIBRARY AND SNDFILE_INCLUDE_DIR)
    target_sources(dusk-studio-tests PRIVATE
        audiofile_roundtrip.cpp
        audiofile_ab_null.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/audiofile/FileReader.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/audiofile/FileWriter.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/audiofile/ThreadedFileWriter.cpp)
    target_include_directories(dusk-studio-tests SYSTEM PRIVATE "${SNDFILE_INCLUDE_DIR}")
    target_link_libraries(dusk-studio-tests PRIVATE "${SNDFILE_LIBRARY}")
    message(STATUS "libsndfile found — audiofile tests enabled")
else()
    message(STATUS "libsndfile not found — audiofile tests skipped (Linux-first, test-only unit)")
endif()

# Console-saturation calibration test + the donor DSP headers it needs
# (BritishEQProcessor.h + its header-only ConsoleSaturation/ADAA/SafeFloat deps).
# Gated on the donor being available, so a no-donor build doesn't try to compile
# a test whose includes can't be resolved.
if(DUSK_PLUGINS_PATH)
    target_sources(dusk-studio-tests PRIVATE console_saturation.cpp)
    target_include_directories(dusk-studio-tests SYSTEM PRIVATE
        ${DUSK_PLUGINS_PATH}/plugins/multi-q
        ${DUSK_PLUGINS_PATH}/plugins/shared)
endif()

target_compile_definitions(dusk-studio-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
    DUSKSTUDIO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Native CLAP host is Linux-only (dlopen/poll) — gate its tests + sources to match
# the app build (DUSKSTUDIO_NATIVE_CLAP). clap_bundle_load also needs dladdr.
if(DUSKSTUDIO_NATIVE_CLAP)
    target_sources(dusk-studio-tests PRIVATE
        clap_bundle_load.cpp
        clap_instance_process.cpp
        clap_ab_nulltest.cpp
        clap_native_slot.cpp
        clap_scanner.cpp
        clap_params.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapBundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapHost.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapInstance.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapScanner.cpp)
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_NATIVE_CLAP=1)
endif()

# Native LV2 host — lilv/suil (ISC), ON when the dev libs are present (detected in
# the top-level CMakeLists; PkgConfig::LILV etc. are GLOBAL imported targets).
if(DUSKSTUDIO_NATIVE_LV2)
    target_sources(dusk-studio-tests PRIVATE
        lv2_bundle_load.cpp
        lv2_instance_process.cpp
        lv2_native_slot.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/lv2/Lv2Bundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/lv2/Lv2Instance.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE PkgConfig::LILV PkgConfig::SUIL PkgConfig::LV2)
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_NATIVE_LV2=1)
endif()

if(DUSKSTUDIO_NATIVE_CLAP OR DUSKSTUDIO_NATIVE_LV2 OR DUSKSTUDIO_NATIVE_VST3)
    target_sources(dusk-studio-tests PRIVATE native_instrument_process.cpp)
endif()

# Native VST3 host — SDK hosting subset (see the dusk-vst3-hosting target in the
# top-level CMakeLists), ON when the external/vst3sdk submodule is initialized.
if(DUSKSTUDIO_NATIVE_VST3)
    target_sources(dusk-studio-tests PRIVATE
        vst3_bundle_load.cpp
        vst3_hostcontext_runloop.cpp
        vst3_instance_process.cpp
        vst3_native_slot.cpp
        vst3_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3Bundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3HostContext.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3Instance.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3Scanner.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE dusk-vst3-hosting)
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_NATIVE_VST3=1)
endif()

target_link_libraries(dusk-studio-tests PRIVATE
    Catch2::Catch2WithMain
    ${CMAKE_DL_LIBS}   # dl: ClapBundle dlopen
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
if(DUSK_PLUGINS_PATH)
    target_sources(dusk-studio-tests PRIVATE
        comp_static_curve.cpp
        comp_timing.cpp
        bus_stereo_link.cpp
        tape_hysteresis.cpp
        bus_strip.cpp
        mastering_chain.cpp
        mastering_digital_eq.cpp
        dsp_stereo_oversampler.cpp
        eq_british_ab.cpp
        tube_multiq_ab.cpp
        comp_core_ab.cpp
        smoke_brickwall_limiter.cpp
        dsp_loudness_meter.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/BrickwallLimiter.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/BusStrip.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/MasteringChain.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/MasteringDigitalEq.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/LoudnessMeter.cpp
        "${DUSK_PLUGINS_PATH}/plugins/shared-dpf/dsp/FourKEQDSP.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/core/UniversalCompressorDSP.cpp"
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

    # projucer-shim satisfies BritishEQProcessor.h's `#include <JuceHeader.h>`
    # (the donor EQ under A/B test in eq_british_ab.cpp); the shim pulls in a
    # few JUCE modules the base test link is missing, added below.
    target_include_directories(dusk-studio-tests SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/external/projucer-shim"
        "${DUSK_PLUGINS_PATH}/plugins/shared"
        "${DUSK_PLUGINS_PATH}/plugins/shared-dpf"
        "${DUSK_PLUGINS_PATH}/plugins/shared/AnalogEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/shared/channel-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-q"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/core"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/HardwareEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/shared-dpf/dsp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source")

    # Modules referenced only transitively by the projucer-shim umbrella header.
    target_link_libraries(dusk-studio-tests PRIVATE
        juce::juce_audio_utils
        juce::juce_data_structures
        juce::juce_events
        juce::juce_gui_extra)

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

# MP3 bounce writer — only when libmp3lame is available (mirrors the app build).
find_path(LAME_INCLUDE_DIR NAMES lame/lame.h)
find_library(LAME_LIBRARY NAMES mp3lame lame)
if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    target_sources(dusk-studio-tests PRIVATE
        lame_mp3_writer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/engine/LameMp3Writer.cpp)
    target_include_directories(dusk-studio-tests PRIVATE "${LAME_INCLUDE_DIR}")
    target_link_libraries(dusk-studio-tests PRIVATE "${LAME_LIBRARY}")
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_LAME=1)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(dusk-studio-tests)
