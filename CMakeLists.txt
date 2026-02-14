cmake_minimum_required(VERSION 3.20)
project(later LANGUAGES C CXX)

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES "Linux")
    message(FATAL_ERROR "
    Unsupported Platform: ${CMAKE_SYSTEM_NAME}
    This project only supports Linux and macOS.
    Aborting configuration.
    ")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

add_executable(later
    src/later.cpp
    src/time_parser.cpp
    src/task.cpp
    src/executor.cpp
    src/storage.cpp
    src/utils.cpp
    src/daemon.cpp
    src/3rdparty/linenoise/linenoise.c
)

target_include_directories(later PRIVATE src)

target_compile_definitions(later PRIVATE FMT_HEADER_ONLY)

set_source_files_properties(src/3rdparty/linenoise/linenoise.c PROPERTIES COMPILE_OPTIONS "-w")