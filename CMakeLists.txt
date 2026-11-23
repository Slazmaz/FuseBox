cmake_minimum_required(VERSION 3.20)
project(fusebox C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)

# stb_image (public domain): PNG/JPEG for art; stb_truetype: the UI's text.
add_library(stb_image STATIC third_party/stb/stb_image.c)
target_include_directories(stb_image PUBLIC third_party/stb)
target_link_libraries(stb_image PRIVATE SDL3::SDL3)
add_library(stb_truetype STATIC third_party/stb/stb_truetype.c)
target_include_directories(stb_truetype PUBLIC third_party/stb)
target_link_libraries(stb_truetype PRIVATE SDL3::SDL3)

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
   src/jobs.c
)
target_include_directories(fusebox_core PUBLIC src)
target_link_libraries(fusebox_core PUBLIC SDL3::SDL3 PRIVATE CURL::libcurl)
if(WIN32)
   target_link_libraries(fusebox_core PRIVATE advapi32 dwmapi)
endif()

add_executable(fusebox src/main.c src/ui.c src/gl.c)
target_link_libraries(fusebox PRIVATE fusebox_core stb_image stb_truetype OpenGL::GL)
# A window app on Windows: no console of its own.
set_target_properties(fusebox PROPERTIES WIN32_EXECUTABLE ON)

# A console app, so other programs can read what it prints.
add_executable(fusebox-cli src/cli.c)
target_link_libraries(fusebox-cli PRIVATE fusebox_core)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
   foreach(t fusebox fusebox-cli fusebox_core)
      target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
   endforeach()
endif()

# The font next to the exe.
add_custom_command(TARGET fusebox POST_BUILD
   COMMAND ${CMAKE_COMMAND} -E copy_directory
      ${CMAKE_SOURCE_DIR}/fonts $<TARGET_FILE_DIR:fusebox>/fonts)

# SDL3, libcurl and their DLLs next to the exes, so they run outside the
# build shell.
if(WIN32)
   foreach(t fusebox fusebox-cli)
      add_custom_command(TARGET ${t} POST_BUILD
         COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:${t}>
            -DDIRS=$<TARGET_FILE_DIR:SDL3::SDL3>
            -P ${CMAKE_SOURCE_DIR}/cmake/copy_dlls.cmake)
   endforeach()
endif()
