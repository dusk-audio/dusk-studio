# JUCE compiles its own lilv, serd, sord, sratom and zix into every binary that
# hosts LV2. Next to the system copies the native host links, the executable's
# definitions win: the native host would call JUCE's lilv, with its default
# search path, and the system libraries would bind to JUCE's older serd, sord
# and zix, whose zix API differs. Where the system libraries are linked, JUCE's
# format uses them too and its own copy is left out.
#
# Both hosts still compile against JUCE's lilv 0.24.12 headers, which come
# before the system ones on the include path. That holds while the system lilv
# exports every function they call and keeps the layout of LilvInstanceImpl,
# the one struct lilv.h reads inline; a call to newer lilv API needs the system
# headers first.
function(duskstudio_link_juce_lv2_to_system_lilv)
    if(TARGET juce_audio_processors_headless)
        set(module juce_audio_processors_headless)
    else()
        set(module juce_audio_processors)
    endif()
    set(libsUnit "${DUSKSTUDIO_JUCE_SOURCE_DIR}/modules/${module}/${module}_lv2_libs.cpp")
    get_filename_component(libsUnit "${libsUnit}" ABSOLUTE)

    get_target_property(sources ${module} INTERFACE_SOURCES)
    list(FIND sources "${libsUnit}" libsIndex)
    if(libsIndex LESS 0)
        message(FATAL_ERROR "Cannot locate JUCE's bundled LV2 libraries unit ${libsUnit}")
    endif()
    list(REMOVE_AT sources ${libsIndex})
    set_property(TARGET ${module} PROPERTY INTERFACE_SOURCES "${sources}")

    pkg_check_modules(SERD REQUIRED IMPORTED_TARGET GLOBAL serd-0)
    target_link_libraries(${module} INTERFACE PkgConfig::LILV PkgConfig::SERD)
    message(STATUS "Dusk Studio: JUCE's LV2 format uses the system lilv")
endfunction()
