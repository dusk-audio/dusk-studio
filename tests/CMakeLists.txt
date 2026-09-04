include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

FetchContent_GetProperties(Catch2)
if(catch2_POPULATED AND IS_DIRECTORY "${catch2_SOURCE_DIR}")
    get_property(testSubdirectories DIRECTORY PROPERTY SUBDIRECTORIES)
    list(FIND testSubdirectories "${catch2_SOURCE_DIR}" catch2DirectoryIndex)
    if(NOT catch2DirectoryIndex EQUAL -1)
        set_property(DIRECTORY "${catch2_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    endif()
endif()

add_executable(dusk-studio-tests
    # Shared native-host foundation (PortLayout / PortBuffers / INativeInstance)
    # is header-only + platform-neutral, so it links in the base target.
    hosting_port_layout.cpp
    hosting_insert_adapter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/hosting/InsertAdapter.cpp
    sfz_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/sfz/SfzCatalog.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/sfz/SfzCatalogEnvelope.cpp
    # Native CLAP tests + sources are added conditionally below.
    plugin_descriptor.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PluginDescriptor.cpp
    plugin_manager_descriptor.cpp
    plugin_scan_watchdog.cpp
    plugin_slot_persistence.cpp
    plugin_slot_lifecycle.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PluginManager.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PluginSlot.cpp
    native_plugin_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/NativePluginCache.cpp
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
    native_editor_owner.cpp
    x11_anti_pattern_guard.cpp
    platform_window_activation.cpp
    clap_editor_lifecycle.cpp
    plugin_state_diagnostics.cpp
    channel_strip_midi_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/ChannelStrip.cpp
    channel_strip_context_rename.cpp
    snap_helpers.cpp
    fade_shapes.cpp
    crossfade.cpp
    clone_plugin_snapshot.cpp
    fade_curve_path.cpp
    file_importer_audio.cpp
    hardware_insert.cpp
    base64_decode.cpp
    legacy_state_base64.cpp
    session_round_trip.cpp
    session_tape_migration.cpp
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
    imgui_gl_loader_win32.cpp
    device_interfaces.cpp
    device_state_blob.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/device/DeviceStateBlob.cpp
    # Native DeviceManager orchestrator (mock-backend suite). Compiled with
    # neither DUSKSTUDIO_HAS_PIPEWIRE nor DUSKSTUDIO_HAS_ALSA, so the owning
    # juce adapters compile out and the TU links JUCE-free; the mock suite
    # injects its own device types via setDeviceTypesForTest.
    device_manager_native.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/device/DeviceManager.cpp
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
    loop_timeline.cpp
    generated_midi_budget.cpp
    session_mcu_roundtrip.cpp
    console_bank_mapping.cpp
    track_meter_source.cpp
    mcu_receiver_decode.cpp
    mcu_controller_emit.cpp
    mcu_fader_taper.cpp
    comp_mode_fanout.cpp
    foundation_message_thread.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/McuReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/McuController.cpp
    ${CMAKE_SOURCE_DIR}/src/foundation/MessageThread.cpp
    multisample_state_roundtrip.cpp
    multisample_hdcc_coalescing.cpp
    bounce_stem_filename.cpp
    bounce_stem_targets.cpp
    dp_importer_parse.cpp
    dp_aligner_align.cpp
    metronome_click.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/Metronome.cpp
    aria_gui_parse.cpp
    aria_bank_selection.cpp
    plugin_host_command_line.cpp
    sf2_reader_parse.cpp
    sf2_to_sfz_convert.cpp
    sf2_preset_sort.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/DuskMultisampleProcessor.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/AriaBank.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/AriaGui.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/Sf2Reader.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/multisample/Sf2ToSfz.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/HardwareInsertSlot.cpp
    file_importer_midi.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FileImporter.cpp
    midi_file_reader.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/midi/MidiFileReader.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DpImporter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DpAligner.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/RecordManager.cpp
    record_loop_take_stacking.cpp
    record_midi_overdub_diff.cpp
    record_offset_placement.cpp
    playback_loop_read.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PlaybackEngine.cpp
    mastering_player_resample.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MasteringPlayer.cpp
    session_missing_sections.cpp
    session_save_as_consolidation.cpp
    session_automation_load_publish.cpp
    session_notepad_sidecar.cpp
    notepad_document.cpp
    notepad_editor_core.cpp
    notepad_chords.cpp
    notepad_theme.cpp
    notepad_graphics_compatibility.cpp
    notepad_first_frame_probe.cpp
    appconfig_store.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/AppConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/NotepadDocument.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/NotepadEditorCore.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/NotepadChords.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiTimeCodeReceiver.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/MidiTimeCodeEmitter.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Session.cpp
    ${CMAKE_SOURCE_DIR}/src/session/SessionSerializer.cpp)

target_include_directories(dusk-studio-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/projucer-shim
    ${CMAKE_SOURCE_DIR}/external/clap/include)
target_include_directories(dusk-studio-tests SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/external/json)

if(WIN32)
    # shell32: CommandLineToArgvW plus SHGetKnownFolderPath; ole32:
    # CoTaskMemFree for the returned known-folder path; advapi32: per-user
    # security descriptors and token checks for the single-instance pipe.
    target_link_libraries(dusk-studio-tests PRIVATE shell32 ole32 advapi32)
endif()

# pffft backend for dusk::audio::Fft, plus its A/B parity suite. The pffft
# object library (include dir + warning silencing) is defined in the root
# CMakeLists and shared with the app target.
target_sources(dusk-studio-tests PRIVATE
    fft_pffft_parity.cpp
    ${CMAKE_SOURCE_DIR}/src/foundation/Fft.cpp)
target_link_libraries(dusk-studio-tests PRIVATE dusk-pffft)

# JUCE-free foundation helpers (dusk::text / dusk::fs / dusk::json) — no external deps.
target_sources(dusk-studio-tests PRIVATE
    foundation_text.cpp
    foundation_text_format.cpp
    foundation_vector_ops.cpp
    foundation_fs.cpp
    foundation_special_locations.cpp
    ${CMAKE_SOURCE_DIR}/src/session/RecentSessions.cpp
    foundation_json.cpp
    foundation_audio_math.cpp
    foundation_int_delay_line.cpp
    foundation_spsc_index_fifo.cpp
    foundation_lagrange_interpolator.cpp
    foundation_windowed_sinc_interpolator.cpp
    foundation_midi_buffer.cpp
    foundation_midi_note_name.cpp
    foundation_planar_buffer.cpp
    midi_ring.cpp
    midi_collector.cpp
    lv2_state_paths.cpp)

# Native ALSA-seq MIDI backend - Linux-only (snd_seq_* in the same libasound the
# audio backend links). The loopback suite drives the real backend path in-
# process and SKIPs itself when /dev/snd/seq is absent (headless CI).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(dusk-studio-tests PRIVATE
        alsa_seq_midi.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/midi/AlsaSeqMidi.cpp
        # ALSA audio backend: the wedged-I/O-thread abandon/park path needs no
        # hardware (test-injected thread body, PCMs never opened).
        alsa_thread_teardown.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/alsa/AlsaAudioIODevice.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE asound)
else()
    # The seam picks its backend on __linux__, so off Linux the JUCE fallback is
    # what MidiDevices.cpp links against. It is the only remaining user of the
    # JUCE MIDI device API in the test binary, so the module is linked here
    # rather than for every platform.
    target_sources(dusk-studio-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src/engine/midi/JuceMidiBackend.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE juce::juce_audio_devices)
endif()

# JUCE-free audio file I/O. Required: the src/ units under test read and write
# through this seam unconditionally. dusk_sndfile is resolved in the top-level
# CMakeLists (config package where available, plain library search otherwise).
target_sources(dusk-studio-tests PRIVATE
    audiofile_roundtrip.cpp
    audiofile_ab_null.cpp
    audiofile_buffered_reader.cpp
    audiofile_drain_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/audiofile/BufferedFileReader.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/audiofile/FileReader.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/audiofile/FileWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/audiofile/ThreadedFileWriter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/audiofile/WriterDrainPool.cpp)
target_link_libraries(dusk-studio-tests PRIVATE dusk_sndfile)
target_link_libraries(dusk-studio-tests PRIVATE dusk_sodium)

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

# The Dear ImGui widget kit and one console strip drawn through it, headlessly: no
# window, no GL and no compositor, so the frame-cost work the GUI tower rests on has a
# regression test that runs everywhere. Gated on the DAF-Widgets checkout the kit lives
# in, the same way the app gates the native notepad on it. Dear ImGui's own translation
# units are compiled here directly - the framework's bridge is what needs DGL, the
# widgets do not.
if(DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR)
    target_sources(dusk-studio-tests PRIVATE
        imgui_widget_kit.cpp
        imgui_panel_controls.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/imgui/DuskTheme.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/imgui/PanelControls.cpp
        ${CMAKE_SOURCE_DIR}/tools/gui-spike/ChannelStripView.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DuskWidgets.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui_draw.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui_tables.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui_widgets.cpp)
    target_include_directories(dusk-studio-tests SYSTEM PRIVATE
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl)
    # Dear ImGui is vendored third-party code and does not build clean under this
    # target's warning set. The app silences it the same way for dgl-opengl3.
    set_source_files_properties(
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui_draw.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui_tables.cpp
        ${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}/opengl/DearImGui/imgui_widgets.cpp
        PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
endif()

target_compile_definitions(dusk-studio-tests PRIVATE
    DUSKSTUDIO_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    $<$<BOOL:${DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS}>:DUSKSTUDIO_JUCE_HAS_FREE_ADD_FORMATS=1>
    DUSKSTUDIO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# Gate native CLAP tests + sources to match the app build. The bundle-load test
# follows the same platform implementation selected by DUSKSTUDIO_NATIVE_CLAP.
if(DUSKSTUDIO_NATIVE_CLAP)
    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_MULTI_BUS_CLAP_FIXTURE_PATH="$<TARGET_FILE:dusk-studio-multi-bus-clap-fixture>")
    add_dependencies(dusk-studio-tests dusk-studio-multi-bus-clap-fixture)

    if(APPLE)
        # The directory-bundle resolver test needs a valid Mach-O image that
        # dlopen can safely inspect. Copying the running test executable aborts
        # inside dyld on current macOS instead of returning a load error.
        add_library(dusk-studio-non-clap-fixture MODULE
            fixtures/non_clap_module.cpp)
        set_target_properties(dusk-studio-non-clap-fixture PROPERTIES
            PREFIX ""
            SUFFIX ".dylib")
        target_compile_features(dusk-studio-non-clap-fixture PRIVATE cxx_std_17)
        target_compile_definitions(dusk-studio-tests PRIVATE
            DUSKSTUDIO_NON_CLAP_FIXTURE_PATH="$<TARGET_FILE:dusk-studio-non-clap-fixture>")
        add_dependencies(dusk-studio-tests dusk-studio-non-clap-fixture)
    endif()
    target_sources(dusk-studio-tests PRIVATE
        clap_bundle_load.cpp
        clap_instance_process.cpp
        clap_ab_nulltest.cpp
        clap_native_slot.cpp
        clap_scanner.cpp
        clap_params.cpp
        clap_host_thread_check.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapBundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapHost.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapInstance.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/clap/ClapScanner.cpp)
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_NATIVE_CLAP=1)
endif()

# Native LV2 host — lilv/suil (ISC), ON when the dev libs are present (detected in
# the top-level CMakeLists; PkgConfig::LILV etc. are GLOBAL imported targets).
if(DUSKSTUDIO_NATIVE_LV2)
    set(DUSKSTUDIO_FILE_STATE_LV2_BUNDLE
        "${CMAKE_CURRENT_BINARY_DIR}/file-state-fixture.lv2")
    set(DUSKSTUDIO_MANY_PATCH_LV2_BUNDLE
        "${CMAKE_CURRENT_BINARY_DIR}/many-patch-fixture.lv2")
    file(MAKE_DIRECTORY "${DUSKSTUDIO_FILE_STATE_LV2_BUNDLE}")
    file(MAKE_DIRECTORY "${DUSKSTUDIO_MANY_PATCH_LV2_BUNDLE}")
    configure_file(fixtures/file_state_manifest.ttl
        "${DUSKSTUDIO_FILE_STATE_LV2_BUNDLE}/manifest.ttl" COPYONLY)
    configure_file(fixtures/file_state_plugin.ttl
        "${DUSKSTUDIO_FILE_STATE_LV2_BUNDLE}/file_state_plugin.ttl" COPYONLY)
    add_library(dusk-studio-file-state-lv2-fixture MODULE
        fixtures/file_state_lv2.cpp)
    set_target_properties(dusk-studio-file-state-lv2-fixture PROPERTIES
        PREFIX ""
        OUTPUT_NAME "file_state_fixture"
        # A generator expression prevents multi-config generators from appending
        # /<Config>, which would put the module somewhere manifest.ttl cannot find.
        LIBRARY_OUTPUT_DIRECTORY "$<1:${DUSKSTUDIO_FILE_STATE_LV2_BUNDLE}>")
    target_compile_features(dusk-studio-file-state-lv2-fixture PRIVATE cxx_std_17)
    target_link_libraries(dusk-studio-file-state-lv2-fixture PRIVATE PkgConfig::LV2)

    set(DUSKSTUDIO_MANY_PATCH_PROPERTY_DEFINITIONS "")
    set(DUSKSTUDIO_MANY_PATCH_WRITABLE_PROPERTIES "")
    foreach(property_index RANGE 0 159)
        string(APPEND DUSKSTUDIO_MANY_PATCH_PROPERTY_DEFINITIONS
            "<urn:duskstudio:test:many-patches#p${property_index}>\n"
            "    a lv2:Parameter ; rdfs:label \"p${property_index}\" ;\n"
            "    rdfs:range atom:Float ; lv2:minimum 0.0 ;\n"
            "    lv2:maximum 200.0 ; lv2:default 0.0 .\n\n")
        if(property_index GREATER 0)
            string(APPEND DUSKSTUDIO_MANY_PATCH_WRITABLE_PROPERTIES ",\n                   ")
        endif()
        string(APPEND DUSKSTUDIO_MANY_PATCH_WRITABLE_PROPERTIES
            "<urn:duskstudio:test:many-patches#p${property_index}>")
    endforeach()
    configure_file(fixtures/many_patch_manifest.ttl
        "${DUSKSTUDIO_MANY_PATCH_LV2_BUNDLE}/manifest.ttl" COPYONLY)
    configure_file(fixtures/many_patch_plugin.ttl.in
        "${DUSKSTUDIO_MANY_PATCH_LV2_BUNDLE}/many_patch_plugin.ttl" @ONLY)
    add_library(dusk-studio-many-patch-lv2-fixture MODULE
        fixtures/many_patch_lv2.cpp)
    set_target_properties(dusk-studio-many-patch-lv2-fixture PROPERTIES
        PREFIX ""
        OUTPUT_NAME "many_patch_fixture"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${DUSKSTUDIO_MANY_PATCH_LV2_BUNDLE}>")
    target_compile_features(dusk-studio-many-patch-lv2-fixture PRIVATE cxx_std_17)
    target_link_libraries(dusk-studio-many-patch-lv2-fixture PRIVATE PkgConfig::LV2)

    target_sources(dusk-studio-tests PRIVATE
        lv2_bundle_load.cpp
        lv2_file_state.cpp
        lv2_patch_feedback.cpp
        lv2_instance_process.cpp
        lv2_native_slot.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/lv2/Lv2Bundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/lv2/Lv2Instance.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/lv2/Lv2Scanner.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE PkgConfig::LILV PkgConfig::SUIL PkgConfig::LV2)
    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_HAS_NATIVE_LV2=1
        DUSKSTUDIO_FILE_STATE_LV2_FIXTURE_PATH="${DUSKSTUDIO_FILE_STATE_LV2_BUNDLE}"
        DUSKSTUDIO_MANY_PATCH_LV2_FIXTURE_PATH="${DUSKSTUDIO_MANY_PATCH_LV2_BUNDLE}")
    add_dependencies(dusk-studio-tests
        dusk-studio-file-state-lv2-fixture
        dusk-studio-many-patch-lv2-fixture)
endif()

if(DUSKSTUDIO_NATIVE_CLAP OR DUSKSTUDIO_NATIVE_LV2 OR DUSKSTUDIO_NATIVE_VST3)
    target_sources(dusk-studio-tests PRIVATE native_instrument_process.cpp)
endif()

# Native VST3 host — SDK hosting subset (see the dusk-vst3-hosting target in the
# top-level CMakeLists), ON when the external/vst3sdk submodule is initialized.
if(DUSKSTUDIO_NATIVE_VST3)
    set(DUSKSTUDIO_VST3SDK_DIR "${CMAKE_SOURCE_DIR}/external/vst3sdk")
    set(public_sdk_SOURCE_DIR "${DUSKSTUDIO_VST3SDK_DIR}/public.sdk")
    list(APPEND CMAKE_MODULE_PATH "${DUSKSTUDIO_VST3SDK_DIR}/cmake/modules")
    set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
    set(SMTG_CREATE_MODULE_INFO OFF CACHE BOOL "" FORCE)
    set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)
    include(SMTG_DetectPlatform)
    smtg_detect_platform()
    include(SMTG_AddVST3Library)

    set_target_properties(dusk-vst3-hosting PROPERTIES POSITION_INDEPENDENT_CODE ON)
    smtg_add_vst3plugin(dusk-studio-runtime-relayout-vst3-fixture
        fixtures/runtime_relayout_vst3.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/common/pluginview.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/main/moduleinit.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/vst/vstbus.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/vst/vstcomponentbase.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/vst/vsteditcontroller.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/vst/vstparameters.cpp
        ${DUSKSTUDIO_VST3SDK_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp)
    target_link_libraries(dusk-studio-runtime-relayout-vst3-fixture
        PRIVATE dusk-vst3-hosting)
    target_compile_definitions(dusk-studio-runtime-relayout-vst3-fixture
        PRIVATE PROJECT_INCLUDES_VSTEDITCONTROLLER=1)
    get_target_property(DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_BUNDLE
        dusk-studio-runtime-relayout-vst3-fixture SMTG_PLUGIN_PACKAGE_PATH)

    target_sources(dusk-studio-tests PRIVATE
        vst3_bundle_load.cpp
        vst3_hostcontext_lifetime.cpp
        vst3_instance_process.cpp
        vst3_native_slot.cpp
        vst3_scanner.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3Bundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3HostContext.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3Instance.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/vst3/Vst3Scanner.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE dusk-vst3-hosting)
    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_HAS_NATIVE_VST3=1
        DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_FIXTURE_PATH="${DUSKSTUDIO_RUNTIME_RELAYOUT_VST3_BUNDLE}")
    add_dependencies(dusk-studio-tests dusk-studio-runtime-relayout-vst3-fixture)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(dusk-studio-tests PRIVATE vst3_hostcontext_runloop.cpp)
    endif()
endif()

# Native Audio Unit host — macOS registry/component API.
if(DUSKSTUDIO_NATIVE_AU)
    target_sources(dusk-studio-tests PRIVATE
        au_native.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/au/AuBundle.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/au/AuHost.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/au/AuInstance.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/au/AuScanner.cpp)
    target_link_libraries(dusk-studio-tests PRIVATE
        ${DUSKSTUDIO_AUDIOTOOLBOX_FRAMEWORK}
        ${DUSKSTUDIO_AUDIOUNIT_FRAMEWORK})
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_NATIVE_AU=1)
endif()

target_link_libraries(dusk-studio-tests PRIVATE
    Catch2::Catch2WithMain
    ${CMAKE_DL_LIBS}   # dl: ClapBundle dlopen
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_audio_processors
    # juce_data_structures brings in juce::ValueTree, which carries the
    # multisample state blob.
    juce::juce_data_structures
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

# Cross-platform single-instance ownership and handoff. The test-only
# dispatcher exercises transport delivery without depending on a GUI event
# loop on the headless macOS runner.
if (UNIX OR WIN32)
    target_sources(dusk-studio-tests PRIVATE
        single_instance_socket_lifecycle.cpp
        ${CMAKE_SOURCE_DIR}/src/util/SingleInstance.cpp)
    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_SINGLE_INSTANCE_TESTING=1)
endif()

# IPC round-trip regression — Linux + Windows + macOS 11+. The test
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
elseif (APPLE)
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
    # The parent pipe endpoint uses overlapped I/O on Windows, so the reader's
    # pending read and the caller's control write can progress concurrently.
    target_sources(dusk-studio-tests PRIVATE
        ipc_control_reply.cpp
        ipc_worker_park.cpp)
    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_HAS_OOP_PLUGINS=1
        DUSKSTUDIO_PLUGIN_HOST_PATH="$<TARGET_FILE:dusk-studio-plugin-host>")
    add_dependencies(dusk-studio-tests dusk-studio-plugin-host)
endif()

target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_TESTS=1)

# UniversalCompressor (multi-comp donor) tests — gated on the donor being
# available. Mirrors the main app's DUSK_PLUGINS_PATH wiring exactly so the
# tests link against the same DSP code paths the channel strip uses.
if(DUSK_PLUGINS_PATH)
    target_sources(dusk-studio-tests PRIVATE
        comp_static_curve.cpp
        comp_timing.cpp
        bus_stereo_link.cpp
        tape_hysteresis.cpp
        tape_core_ab.cpp
        bus_strip.cpp
        mastering_chain.cpp
        mastering_digital_eq.cpp
        # The mastering stage's native panels draw the chain through the widget kit, so
        # their headless suite needs the kit as well as the donor DSP this block gates.
        $<$<BOOL:${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}>:imgui_mastering_views.cpp>
        $<$<BOOL:${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}>:${CMAKE_SOURCE_DIR}/src/ui/imgui/MasteringEqView.cpp>
        $<$<BOOL:${DUSKSTUDIO_DAF_WIDGETS_SOURCE_DIR}>:${CMAKE_SOURCE_DIR}/src/ui/imgui/MasteringLimiterView.cpp>
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
        "${DUSKSTUDIO_DONOR_DSP_DIR}/FourKEQDSP.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/multicomp.cpp"
        "${DUSKSTUDIO_COMPRESSOR_CORE_SOURCE}"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/AnalogLookAndFeel.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/EnhancedCompressorEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/shared/LEDMeter.cpp"
        # tape_core_ab.cpp A/Bs the framework-free core against the JUCE donor,
        # so both sides link here. The donor's createEditor() pulls its editor
        # and GUI widgets in behind it.
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/core/TapeMachineDSP.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginEditor.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/ImprovedTapeEmulation.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/AnalogVUMeter.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/TapeReelComponent.cpp"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI/TapeMachineLookAndFeel.cpp")

    # Same createPluginFilter rename the main app applies - both donors define
    # it as their plugin entry point and neither is ever called here.
    set_source_files_properties(
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/PluginProcessor.cpp"
        PROPERTIES
        COMPILE_DEFINITIONS "createPluginFilter=createTapeMachinePluginFilter")

    # Required only here: the tests compile TapeMachine's GUI/AnalogVUMeter.cpp
    # alongside multi-comp's, and both declare a global AnalogVUMeter. Renaming
    # the multi-comp one in-translation-unit keeps the donor untouched. The app
    # no longer links TapeMachine's editor, so it does not need this.
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
        "${DUSKSTUDIO_DONOR_SHARED_DIR}"
        "${DUSK_PLUGINS_PATH}/plugins/shared/AnalogEmulation"
        "${DUSK_PLUGINS_PATH}/plugins/shared/channel-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-q"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/core"
        "${DUSK_PLUGINS_PATH}/plugins/multi-comp/HardwareEmulation"
        "${DUSKSTUDIO_DONOR_DSP_DIR}"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source"
        "${DUSK_PLUGINS_PATH}/plugins/TapeMachine/Source/GUI")

    # Modules referenced only transitively by the projucer-shim umbrella header.
    target_link_libraries(dusk-studio-tests PRIVATE
        juce::juce_audio_utils
        juce::juce_data_structures
        juce::juce_events
        juce::juce_gui_extra)

    target_compile_definitions(dusk-studio-tests PRIVATE
        DUSKSTUDIO_HAS_DUSK_DSP=1
        # Projucer-style macros the donor sources reference unguarded.
        # Mirror the main app's defaults — the values are not load-bearing
        # for tests; they just need to exist.
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JucePlugin_Name="DuskStudio_Comp_Tests"
        JucePlugin_Manufacturer="Marc Korte"
        JucePlugin_IsMidiEffect=0
        JucePlugin_IsSynth=0
        JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0)
endif()

# The native path-opening regression runs even without LAME; encoding coverage
# is enabled when libmp3lame is available (mirrors the app build).
target_sources(dusk-studio-tests PRIVATE
    lame_mp3_writer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/engine/LameMp3Writer.cpp)
find_path(LAME_INCLUDE_DIR NAMES lame/lame.h)
find_library(LAME_LIBRARY NAMES mp3lame lame)
if(LAME_INCLUDE_DIR AND LAME_LIBRARY)
    target_include_directories(dusk-studio-tests PRIVATE "${LAME_INCLUDE_DIR}")
    target_link_libraries(dusk-studio-tests PRIVATE "${LAME_LIBRARY}")
    target_compile_definitions(dusk-studio-tests PRIVATE DUSKSTUDIO_HAS_LAME=1)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
# The environment-gated ALSA-seq loopback tests SKIP() on headless CI. Run in
# isolation (catch_discover_tests filters each case by exact name) a skip-only
# Catch2 run exits 4, which ctest scores as a failure. --allow-running-no-tests
# (Catch2 >= 3.0.1; v3.5.4 is pinned above) makes an all-skipped run exit 0
# while a real failure still returns its assertion count. Scope it to the [alsa]
# tag so an unmatched/stale filter on any other test still fails.
catch_discover_tests(dusk-studio-tests
    TEST_SPEC "[alsa]"
    EXTRA_ARGS --allow-running-no-tests)
catch_discover_tests(dusk-studio-tests
    TEST_SPEC "~[alsa]")

# Exercise the real release-version insertion script against an isolated fixture,
# and pin the private-release fan-in and single-publisher contract. The release
# script is Unix-only.
if(UNIX)
    find_program(DUSKSTUDIO_BASH_EXECUTABLE bash)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(DUSKSTUDIO_BASH_EXECUTABLE AND Python3_Interpreter_FOUND)
        add_test(NAME release-mechanics-contract
            COMMAND "${DUSKSTUDIO_BASH_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/release_mechanics.sh"
                    "${CMAKE_SOURCE_DIR}"
                    "${Python3_EXECUTABLE}")
    else()
        message(WARNING "release-mechanics contract test skipped: bash or python3 not found")
    endif()
endif()
