include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(focal-tests
    smoke_brickwall_limiter.cpp
    session_tick_conversion.cpp
    atomic_park.cpp
    embedded_modal_close_safety.cpp
    x11_anti_pattern_guard.cpp
    snap_helpers.cpp
    fade_shapes.cpp
    crossfade.cpp
    file_importer_audio.cpp
    hardware_insert.cpp
    session_round_trip.cpp
    session_corrupted_recovery.cpp
    session_format_version.cpp
    recording_byte_accuracy.cpp
    midi_fifo_overflow.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/BrickwallLimiter.cpp
    ${CMAKE_SOURCE_DIR}/src/dsp/HardwareInsertSlot.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FileImporter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/RecordManager.cpp
    ${CMAKE_SOURCE_DIR}/src/session/Session.cpp
    ${CMAKE_SOURCE_DIR}/src/session/SessionSerializer.cpp)

target_include_directories(focal-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/projucer-shim)

target_compile_definitions(focal-tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    $<$<BOOL:${FOCAL_JUCE_HAS_FREE_ADD_FORMATS}>:FOCAL_JUCE_HAS_FREE_ADD_FORMATS=1>
    FOCAL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(focal-tests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_audio_basics
    # juce_audio_devices required by SessionSerializer.cpp for
    # juce::MidiInput::getAvailableDevices() — resolves Track::
    # midiInputIdentifier back to an index on session load. Removing
    # this triggers an ld undefined-reference on the MidiInput /
    # MidiOutput statics.
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_dsp
    juce::juce_graphics
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# IPC round-trip regression — Linux only because the IPC uses
# memfd_create + futex (no macOS / Windows port yet). The test spawns
# the focal-plugin-host child in --ipc-stub mode, so we hard-depend on
# that target being built first. FOCAL_PLUGIN_HOST_PATH is resolved via
# a generator expression so it points to wherever CMake actually puts
# the binary in the current build configuration.
if (UNIX AND NOT APPLE)
    target_sources(focal-tests PRIVATE
        ipc_stub_round_trip.cpp
        ${CMAKE_SOURCE_DIR}/src/engine/ipc/RemotePluginConnection.cpp)
    target_compile_definitions(focal-tests PRIVATE
        FOCAL_PLUGIN_HOST_PATH="$<TARGET_FILE:focal-plugin-host>")
    add_dependencies(focal-tests focal-plugin-host)
endif()

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(focal-tests)
