cmake_minimum_required(VERSION 3.20)
project(later)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

set_source_files_properties(
    src/3rdparty/argparse/argparse.c
    src/3rdparty/linenoise/linenoise.c
    PROPERTIES COMPILE_FLAGS "-w")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_compile_definitions(_DARWIN_C_SOURCE)
endif()

add_executable(later
    src/later.c
    src/util.c
    src/exec.c
    src/store.c
    src/action.c
    src/strvec.c
    src/daemon.c
    src/timefmt.c
    src/3rdparty/argparse/argparse.c
    src/3rdparty/linenoise/linenoise.c
)

target_include_directories(later PRIVATE src)
