cmake_minimum_required(VERSION 3.20)
project(EERecomp VERSION 0.1.0 LANGUAGES CXX)

# Default build type for single-config generators (Ninja/Make).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EE_BUILD_TOOLS "Build command-line tools" ON)
option(EE_BUILD_TESTS "Build unit tests" ON)
option(EE_BUILD_STUDIO "Build the ee-studio launcher GUI" ON)
option(EE_BUILD_LAUNCHER "Build the per-game release launcher template" ON)
option(EE_BUILD_ETOOLS "Build the ee-tools GUI (disasm/analyze/recomp)" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /wd4100 /wd4189)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(libs/ee-base)
add_subdirectory(libs/elf)
add_subdirectory(libs/r5900)
add_subdirectory(libs/analysis)
add_subdirectory(libs/codegen)
add_subdirectory(runtime)

# Acquire SDL3 once for all GUI apps (ee-studio, game-launcher, ee-tools).
# Either uses a system/vendored SDL3 or fetches/builds it from source when
# EE_FETCH_SDL3 is ON (default). Sets EE_HAS_SDL3 and provides SDL3::SDL3.
# See cmake/ee_sdl3.cmake for details and the -DEE_FETCH_SDL3=OFF escape hatch.
if(EE_BUILD_STUDIO OR EE_BUILD_LAUNCHER OR EE_BUILD_ETOOLS)
  list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
  include(ee_sdl3)
endif()

if(EE_BUILD_TOOLS)
  add_subdirectory(tools/ee-disasm)
  add_subdirectory(tools/ee-analyze)
  add_subdirectory(tools/ee-recomp)
endif()

if(EE_BUILD_STUDIO)
  add_subdirectory(app/ee-studio)
endif()

if(EE_BUILD_LAUNCHER)
  add_subdirectory(app/game-launcher)
endif()

if(EE_BUILD_ETOOLS)
  add_subdirectory(app/ee-tools)
endif()

if(EE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
