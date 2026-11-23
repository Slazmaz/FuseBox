cmake_minimum_required(VERSION 3.20)
project(fusebox C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(SDL3 REQUIRED CONFIG)
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
   src/disk.c
   src/jobs.c
)
target_include_directories(fusebox_core PUBLIC src)
target_link_libraries(fusebox_core PUBLIC SDL3::SDL3 PRIVATE CURL::libcurl)
if(WIN32)
   target_link_libraries(fusebox_core PRIVATE advapi32 dwmapi)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
   foreach(t fusebox_core)
      target_compile_options(${t} PRIVATE -Wall -Wextra -Wno-unused-parameter)
   endforeach()
endif()
