# SheenBidi (external/sheenbidi, tag v3.0.0, Apache-2.0) resolves the Unicode
# Bidirectional Algorithm for the names the native panels draw. Its unity source is
# the whole library; SYSTEM includes and no warnings keep it out of ours.
#
# JUCE's graphics module compiles its own, older SheenBidi with the same C symbol
# names into the same binaries. Every public function is renamed here, through a
# header generated from SheenBidi's own declarations, so the two cannot collide or
# resolve to each other. Code that calls SheenBidi includes <DuskSheenBidi.h>, never
# the upstream header on its own.
set(_dusk_sheenbidi_dir "${CMAKE_CURRENT_SOURCE_DIR}/external/sheenbidi")
if(NOT EXISTS "${_dusk_sheenbidi_dir}/Source/SheenBidi.c")
    message(FATAL_ERROR
        "SheenBidi is missing at external/sheenbidi - run: git submodule update --init external/sheenbidi")
endif()

file(GLOB _dusk_sheenbidi_headers "${_dusk_sheenbidi_dir}/Headers/SheenBidi/*.h")
set(_dusk_sheenbidi_names "")
foreach(_header IN LISTS _dusk_sheenbidi_headers)
    file(READ "${_header}" _text)
    string(REGEX MATCHALL "SB_PUBLIC[^;(#]*[ *]SB[A-Za-z0-9_]+\\(" _declarations "${_text}")
    foreach(_declaration IN LISTS _declarations)
        string(REGEX MATCH "SB[A-Za-z0-9_]+\\($" _name "${_declaration}")
        string(REGEX REPLACE "\\($" "" _name "${_name}")
        list(APPEND _dusk_sheenbidi_names "${_name}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _dusk_sheenbidi_names)
list(SORT _dusk_sheenbidi_names)
list(LENGTH _dusk_sheenbidi_names _dusk_sheenbidi_count)
if(_dusk_sheenbidi_count LESS 40)
    message(FATAL_ERROR "Found only ${_dusk_sheenbidi_count} SheenBidi functions to rename in ${_dusk_sheenbidi_dir}/Headers")
endif()

set(_dusk_sheenbidi_generated "${CMAKE_CURRENT_BINARY_DIR}/dusk_sheenbidi")
set(_dusk_sheenbidi_header "#pragma once\n\n")
foreach(_name IN LISTS _dusk_sheenbidi_names)
    string(APPEND _dusk_sheenbidi_header "#define ${_name} DuskStudio_${_name}\n")
endforeach()
string(APPEND _dusk_sheenbidi_header "\n#include <SheenBidi/SheenBidi.h>\n")
file(CONFIGURE OUTPUT "${_dusk_sheenbidi_generated}/DuskSheenBidi.h" CONTENT "${_dusk_sheenbidi_header}" @ONLY)
file(CONFIGURE OUTPUT "${_dusk_sheenbidi_generated}/DuskSheenBidi.c"
     CONTENT "#include <DuskSheenBidi.h>\n#include \"${_dusk_sheenbidi_dir}/Source/SheenBidi.c\"\n" @ONLY)

add_library(dusk_sheenbidi STATIC "${_dusk_sheenbidi_generated}/DuskSheenBidi.c")
target_compile_definitions(dusk_sheenbidi PRIVATE SB_CONFIG_UNITY)
target_include_directories(dusk_sheenbidi SYSTEM PUBLIC "${_dusk_sheenbidi_generated}" "${_dusk_sheenbidi_dir}/Headers")
target_include_directories(dusk_sheenbidi PRIVATE "${_dusk_sheenbidi_dir}/Source")
target_compile_options(dusk_sheenbidi PRIVATE "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>" "$<$<C_COMPILER_ID:MSVC>:/utf-8>")
set_target_properties(dusk_sheenbidi PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(dusk_bidi_text STATIC src/foundation/BidiText.cpp)
target_compile_features(dusk_bidi_text PUBLIC cxx_std_17)
target_link_libraries(dusk_bidi_text PRIVATE dusk_sheenbidi)
set_target_properties(dusk_bidi_text PROPERTIES POSITION_INDEPENDENT_CODE ON)
