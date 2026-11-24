cmake_minimum_required(VERSION 3.20)
project(fusebox VERSION 0.1.2 LANGUAGES C)

# The version, above, is what updates go by (update.c).
if(NOT PROJECT_VERSION_PATCH)
   set(PROJECT_VERSION_PATCH 0)
endif()
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# For others' computers (Windows; on Linux tools/make_linux.sh): all but what every
# one has is linked in, so there's nothing to come along.
option(FUSEBOX_PORTABLE "Link libraries in" ${WIN32})
if(FUSEBOX_PORTABLE)
   if(WIN32)
      set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
      # What a build from when they were DLLs found.
      get_cmake_property(cached CACHE_VARIABLES)
      foreach(name IN LISTS cached)
         if("$CACHE{${name}}" MATCHES "[.]dll[.]a$")
            unset(${name} CACHE)
            set(was_dlls TRUE)
         endif()
      endforeach()
      foreach(name IN LISTS cached)
         if(was_dlls AND name MATCHES "^__pkg_config_")
            unset(${name} CACHE)
         endif()
      endforeach()
   else()
      set(CMAKE_FIND_LIBRARY_SUFFIXES .a .so)
   endif()
   set(PKG_CONFIG_ARGN --static CACHE STRING "" FORCE)
endif()

find_package(SDL3 REQUIRED CONFIG)
set(SDL_LIB SDL3::SDL3)
if(FUSEBOX_PORTABLE AND TARGET SDL3::SDL3-static)
   set(SDL_LIB SDL3::SDL3-static)
endif()
find_package(OpenGL REQUIRED)
# ROMs in a .zip, and compressed discs (RVZ).
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
# Windows': its own package says it's the DLL.
if(WIN32)
   pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
   set(CURL_LIB PkgConfig::CURL)
else()
   find_package(CURL REQUIRED)
   set(CURL_LIB CURL::libcurl)
endif()
# WebP: animated art.
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebpdemux libwebp)
# VP8: SteamGridDB's previews of it. Windows: linked in, no DLL of its own.
pkg_check_modules(VPX REQUIRED IMPORTED_TARGET vpx)
if(WIN32)
   find_library(VPX_STATIC NAMES libvpx.a HINTS ${VPX_LIBDIR} REQUIRED)
   set(VPX_LIB ${VPX_STATIC})
else()
   set(VPX_LIB PkgConfig::VPX)
endif()

# stb_image (public domain): PNG/JPEG for art; stb_truetype: the UI's text.
add_library(stb_image STATIC third_party/stb/stb_image.c)
target_include_directories(stb_image PUBLIC third_party/stb)
target_link_libraries(stb_image PRIVATE ${SDL_LIB})
add_library(stb_truetype STATIC third_party/stb/stb_truetype.c)
target_include_directories(stb_truetype PUBLIC third_party/stb)
target_link_libraries(stb_truetype PRIVATE ${SDL_LIB})

# rcheevos (MIT): only how RetroAchievements hashes ROMs.
add_library(rcheevos STATIC
   third_party/rcheevos/src/rc_compat.c
   third_party/rcheevos/src/rc_util.c
   third_party/rcheevos/src/rhash/aes.c
   third_party/rcheevos/src/rhash/cdreader.c
   third_party/rcheevos/src/rhash/hash.c
   third_party/rcheevos/src/rhash/hash_disc.c
   third_party/rcheevos/src/rhash/hash_encrypted.c
   third_party/rcheevos/src/rhash/hash_rom.c
   third_party/rcheevos/src/rhash/hash_zip.c
   third_party/rcheevos/src/rhash/md5.c
)
target_include_directories(rcheevos
   PUBLIC third_party/rcheevos/include
   PRIVATE third_party/rcheevos/src)

# The library, SteamGridDB, the scanners and Steam: shared by the window
# and fusebox-cli.
add_library(fusebox_core STATIC
   src/http.c
   src/credentials.c
   src/json.c
   src/vdf.c
   src/library.c
   src/sgdb.c
   src/platform.c
   src/steam.c
   src/scan.c
   src/disk.c
   src/roms.c
   src/jobs.c
   src/ra.c
   src/rahash.c
   src/disc.c
   src/crypto.c
   src/steamui.c
   src/background.c
   src/history.c
   src/lan.c
   src/sockets.c
   src/stream.c
   src/vlan.c
   src/inject.c
   src/inject_linux.c
   src/tunnel.c
   src/steamnet.c
   src/steamlib.c
   src/steamlib_linux.c
   src/session.c
   src/update.c
)
target_include_directories(fusebox_core PUBLIC src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(fusebox_core PUBLIC ${SDL_LIB} PRIVATE ${CURL_LIB} rcheevos ZLIB::ZLIB PkgConfig::ZSTD ${CMAKE_DL_LIBS})
if(WIN32)
   # More sockets to wait on than Windows' 64.
   target_compile_definitions(fusebox_core PRIVATE FD_SETSIZE=1024 WIN32_LEAN_AND_MEAN)
   target_link_libraries(fusebox_core PRIVATE advapi32 dwmapi userenv ws2_32 iphlpapi)
endif()

add_executable(fusebox src/main.c src/ui.c src/gl.c src/anim.c)
target_link_libraries(fusebox PRIVATE fusebox_core stb_image stb_truetype OpenGL::GL PkgConfig::WEBP ${VPX_LIB})
# A window app on Windows: no console of its own.
set_target_properties(fusebox PROPERTIES WIN32_EXECUTABLE ON)

# A console app, so other programs can read what it prints.
add_executable(fusebox-cli src/cli.c)
target_link_libraries(fusebox-cli PRIVATE fusebox_core)

if(WIN32 AND FUSEBOX_PORTABLE)
   target_link_options(fusebox PRIVATE -static -s)
   target_link_options(fusebox-cli PRIVATE -static -s)
endif()

# In games that are played with friends (Windows: by MinHook, BSD), in lib.
if(WIN32)
   add_library(minhook STATIC
      third_party/minhook/src/buffer.c
      third_party/minhook/src/hook.c
      third_party/minhook/src/trampoline.c
      third_party/minhook/src/hde/hde64.c)
   target_include_directories(minhook PUBLIC third_party/minhook/include)
   add_library(fuseboxlan SHARED src/lanhook.c)
   set_target_properties(fuseboxlan PROPERTIES PREFIX "")
   target_compile_definitions(fuseboxlan PRIVATE WIN32_LEAN_AND_MEAN)
   target_link_options(fuseboxlan PRIVATE -static -s)
   target_link_libraries(fuseboxlan PRIVATE minhook ws2_32 iphlpapi)
else()
   # Started with the games (LD_PRELOAD): nothing of it but its hooks is to be seen.
   add_library(fuseboxlan SHARED src/lanhook_linux.c)
   set_target_properties(fuseboxlan PROPERTIES C_VISIBILITY_PRESET hidden)
   target_link_libraries(fuseboxlan PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(fuseboxlan PROPERTIES
   RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
   LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
add_dependencies(fusebox fuseboxlan)

# The app icon and version, in both exes.
if(WIN32)
   enable_language(RC)
   target_sources(fusebox PRIVATE src/fusebox.rc)
   target_sources(fusebox-cli PRIVATE src/fusebox.rc)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
   set(OWN fusebox fusebox-cli fusebox_core fuseboxlan)
   foreach(t ${OWN})
      target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
   endforeach()
endif()

# The font next to the exe.
add_custom_command(TARGET fusebox POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/fonts $<TARGET_FILE_DIR:fusebox>/fonts)
