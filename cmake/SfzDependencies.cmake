add_library(dusk_sfz_dependencies INTERFACE)
target_link_libraries(dusk_sfz_dependencies INTERFACE dusk_sodium)

if(WIN32)
    find_package(CURL CONFIG REQUIRED)
    find_package(utf8proc CONFIG REQUIRED)
    find_package(ZLIB REQUIRED)

    # Prefix-only Windows builds do not run vcpkg's FindLibArchive wrapper,
    # which supplies the static archive's zlib dependency and library name.
    find_library(LibArchive_LIBRARY NAMES archive_static archive libarchive REQUIRED)
    find_package(LibArchive REQUIRED)
    find_library(DUSKSTUDIO_LIBARCHIVE_DEBUG_LIBRARY
        NAMES archive_static archive libarchive
        PATHS ${CMAKE_PREFIX_PATH}
        PATH_SUFFIXES debug/lib
        NO_DEFAULT_PATH)
    if(DUSKSTUDIO_LIBARCHIVE_DEBUG_LIBRARY)
        set_target_properties(LibArchive::LibArchive PROPERTIES
            IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
            IMPORTED_LOCATION_RELEASE "${LibArchive_LIBRARY}"
            IMPORTED_LOCATION_DEBUG "${DUSKSTUDIO_LIBARCHIVE_DEBUG_LIBRARY}")
    endif()
    target_compile_definitions(dusk_sfz_dependencies INTERFACE LIBARCHIVE_STATIC)
    target_link_libraries(dusk_sfz_dependencies INTERFACE
        CURL::libcurl LibArchive::LibArchive ZLIB::ZLIB utf8proc::utf8proc)
elseif(APPLE)
    find_package(CURL 7.62.0 REQUIRED)
    find_package(LibArchive 3.4.0 REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(dusk_sfz_dependencies INTERFACE
        CURL::libcurl LibArchive::LibArchive ZLIB::ZLIB)

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DUSKSTUDIO_UTF8PROC QUIET IMPORTED_TARGET GLOBAL libutf8proc>=2.11.3)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DUSKSTUDIO_CURL REQUIRED IMPORTED_TARGET GLOBAL libcurl>=7.62.0)
    pkg_check_modules(DUSKSTUDIO_LIBARCHIVE REQUIRED IMPORTED_TARGET GLOBAL libarchive>=3.4.0)
    pkg_check_modules(DUSKSTUDIO_UTF8PROC QUIET IMPORTED_TARGET GLOBAL libutf8proc>=2.11.3)
    target_link_libraries(dusk_sfz_dependencies INTERFACE
        PkgConfig::DUSKSTUDIO_CURL PkgConfig::DUSKSTUDIO_LIBARCHIVE)
endif()

if(NOT WIN32)
    if(DUSKSTUDIO_UTF8PROC_FOUND)
        target_link_libraries(dusk_sfz_dependencies INTERFACE PkgConfig::DUSKSTUDIO_UTF8PROC)
    else()
        include(FetchContent)
        FetchContent_Declare(dusk_utf8proc
            GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
            GIT_TAG e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78
            SOURCE_SUBDIR dusk-source-only)
        FetchContent_MakeAvailable(dusk_utf8proc)
        add_library(dusk_utf8proc STATIC "${dusk_utf8proc_SOURCE_DIR}/utf8proc.c")
        target_include_directories(dusk_utf8proc SYSTEM PUBLIC "${dusk_utf8proc_SOURCE_DIR}")
        target_compile_definitions(dusk_utf8proc PUBLIC UTF8PROC_STATIC)
        target_compile_features(dusk_utf8proc PRIVATE c_std_99)
        set_target_properties(dusk_utf8proc PROPERTIES POSITION_INDEPENDENT_CODE ON)
        target_link_libraries(dusk_sfz_dependencies INTERFACE dusk_utf8proc)
        message(STATUS "utf8proc: pinned 2.11.3 source (system development package absent or older)")
    endif()
endif()
