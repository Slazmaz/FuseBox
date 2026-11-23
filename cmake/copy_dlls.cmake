# Copies every DLL EXE needs from DIRS (the toolchain's bin) next to it,
# including the DLLs those DLLs need. Windows' own DLLs are left out.
if(POLICY CMP0207)
   cmake_policy(SET CMP0207 NEW)
endif()

file(GET_RUNTIME_DEPENDENCIES
   EXECUTABLES "${EXE}"
   RESOLVED_DEPENDENCIES_VAR deps
   UNRESOLVED_DEPENDENCIES_VAR unresolved
   CONFLICTING_DEPENDENCIES_PREFIX conflicts
   DIRECTORIES ${DIRS}
   PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
   POST_EXCLUDE_REGEXES "[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]")

get_filename_component(dest "${EXE}" DIRECTORY)

# A DLL copied by an earlier build is found next to the exe and in DIRS:
# take the toolchain's.
foreach(name IN LISTS conflicts_FILENAMES)
   foreach(path IN LISTS conflicts_${name})
      get_filename_component(dir "${path}" DIRECTORY)
      if(NOT dir STREQUAL dest)
         list(APPEND deps "${path}")
         break()
      endif()
   endforeach()
endforeach()

foreach(dep IN LISTS deps)
   file(COPY "${dep}" DESTINATION "${dest}")
endforeach()
