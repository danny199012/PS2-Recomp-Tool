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

if(EE_BUILD_TOOLS)
  add_subdirectory(tools/ee-disasm)
  add_subdirectory(tools/ee-analyze)
  add_subdirectory(tools/ee-recomp)
endif()

if(EE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)

if(EE_BUILD_TOOLS)
  add_subdirectory(app/ee-studio)
endif()
endif()
