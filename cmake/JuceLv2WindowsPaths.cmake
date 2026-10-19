function(duskstudio_lv2_replace variable before after)
    string(FIND "${${variable}}" "${before}" index)
    if(index LESS 0)
        message(FATAL_ERROR "Unrecognized bundled LV2 filesystem code; cannot replace: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" result "${${variable}}")
    set(${variable} "${result}" PARENT_SCOPE)
endfunction()

function(duskstudio_juce_lv2_windows_paths moduleDirectory copyDirectory)
    set(prefix "duskstudio::lv2WindowsPaths::")
    foreach(relative IN ITEMS serd/src/system.c lilv/src/filesystem.c lilv/src/util.c
                              lilv/src/state.c lilv/src/lib.c lilv/src/world.c)
        set(original "${moduleDirectory}/format_types/LV2_SDK/${relative}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
        if(relative STREQUAL "lilv/src/state.c")
            set(source "${state}")
        else()
            file(READ "${original}" source)
        endif()

        string(REGEX REPLACE "([=(!? \n])fopen\\(" "\\1${prefix}openFile(" source "${source}")
        string(REGEX REPLACE "([=(!? \n])remove\\(" "\\1${prefix}removeFile(" source "${source}")
        string(REGEX REPLACE "([=(! \n])stat\\(" "\\1${prefix}fileInfo(" source "${source}")
        string(REPLACE "struct stat " "struct _stat64 " source "${source}")

        if(relative STREQUAL "lilv/src/filesystem.c")
            duskstudio_lv2_replace(source "_mkdir(path)" "${prefix}makeDirectory(path)")
            duskstudio_lv2_replace(source "getcwd(NULL, 0)" "${prefix}currentDirectory()")
            duskstudio_lv2_replace(source "access(path, F_OK)" "${prefix}pathAccess(path, F_OK)")
            duskstudio_lv2_replace(source "CreateHardLink(newpath, oldpath, 0)" "${prefix}hardLink(newpath, oldpath, 0)")
            duskstudio_lv2_replace(source "RemoveDirectory(path)" "${prefix}removeDirectory(path)")
            duskstudio_lv2_replace(source "static off_t\nlilv_file_size" "static int64_t\nlilv_file_size")
            set(before [=[  DWORD len = GetTempPath(0, NULL);
  char* buf = (char*)calloc(len, 1);
  if (GetTempPath(len, buf) == 0) {
    free(buf);
    return NULL;
  }

  return buf;]=])
            duskstudio_lv2_replace(source "${before}" "  return ${prefix}temporaryDirectory();")
            set(before [=[  char* out = (char*)malloc(MAX_PATH);
  GetFullPathName(path, MAX_PATH, out, NULL);
  return out;]=])
            duskstudio_lv2_replace(source "${before}" "  return ${prefix}absolutePath(path);")
            set(before [=[  char*           pat = lilv_path_join(path, "*");
  WIN32_FIND_DATA fd;
  HANDLE          fh = FindFirstFile(pat, &fd);
  if (fh != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
        f(path, fd.cFileName, data);
      }
    } while (FindNextFile(fh, &fd));
  }
  FindClose(fh);
  free(pat);]=])
            duskstudio_lv2_replace(source "${before}" "  ${prefix}forEachDirectory(path, data, f);")
        elseif(relative STREQUAL "lilv/src/lib.c")
            duskstudio_lv2_replace(source "dlopen(lib_path, RTLD_NOW)" "${prefix}loadLibrary(lib_path)")
        elseif(relative STREQUAL "lilv/src/world.c")
            duskstudio_lv2_replace(source "dlopen(lib_path, RTLD_LAZY)" "${prefix}loadLibrary(lib_path)")
        elseif(relative STREQUAL "lilv/src/util.c")
            set(before [=[  char* out = (char*)malloc(MAX_PATH);
  ExpandEnvironmentStrings(path, out, MAX_PATH);]=])
            duskstudio_lv2_replace(source "${before}" "  char* out = ${prefix}expandEnvironment(path);")
        elseif(relative STREQUAL "serd/src/system.c")
            duskstudio_lv2_replace(source "#include \"serd_internal.h\""
                "#include \"${moduleDirectory}/format_types/LV2_SDK/serd/src/serd_internal.h\"")
            duskstudio_lv2_replace(source "#include \"system.h\""
                "#include \"${moduleDirectory}/format_types/LV2_SDK/serd/src/system.h\"")
        endif()

        if(relative STREQUAL "lilv/src/state.c")
            set(state "${source}")
        else()
            set(copy "${copyDirectory}/${relative}")
            get_filename_component(directory "${copy}" DIRECTORY)
            file(MAKE_DIRECTORY "${directory}")
            file(CONFIGURE OUTPUT "${copy}" CONTENT "${source}" @ONLY)
            duskstudio_lv2_replace(support "#include \"${relative}\"" "#include \"${copy}\"")
        endif()
    endforeach()

    set(header "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/JuceLv2WindowsPaths.h")
    duskstudio_lv2_replace(support "extern \"C\"" "#include \"${header}\"\n\nextern \"C\"")
    set(state "${state}" PARENT_SCOPE)
    set(support "${support}" PARENT_SCOPE)
endfunction()
