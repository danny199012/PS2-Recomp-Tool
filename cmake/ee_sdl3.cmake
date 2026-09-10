# SPDX-License-Identifier: GPL-3.0-only
#
# Shared SDL3 acquisition for the GUI apps (ee-tools, ee-studio, game-launcher).
# include()-ed once from the top-level CMakeLists *before* the app
# add_subdirectory() calls, so the resulting SDL3::SDL3 target and the
# EE_HAS_SDL3 variable are visible to all of them.
#
# Resolution order:
#   1. A system/vendored SDL3 found via find_package(SDL3) -- e.g. the official
#      SDL3-devel-*.zip with SDL3_DIR set, or a distro package. Used as-is.
#   2. Otherwise, if EE_FETCH_SDL3 is ON (the default), download and build SDL3
#      from source via FetchContent (static library, tests/examples/install
#      disabled). This lets the GUI apps build out-of-the-box on a clean clone
#      at the cost of a longer first configure/build.
#   3. Otherwise (EE_FETCH_SDL3=OFF and no system SDL3), EE_HAS_SDL3 stays OFF
#      and each app compiles a visible "SDL3 required" stub instead of a GUI.
#
# After include()-ing this file, callers may rely on:
#   - EE_HAS_SDL3          (ON/OFF)
#   - the SDL3::SDL3 target (present only when EE_HAS_SDL3 is ON)

include_guard(GLOBAL)

option(EE_FETCH_SDL3 "Download and build SDL3 from source when not installed" ON)

set(EE_HAS_SDL3 OFF)

find_package(SDL3 QUIET)
if(SDL3_FOUND OR TARGET SDL3::SDL3)
  set(EE_HAS_SDL3 ON)
  message(STATUS "ee: using system/vendored SDL3 (EE_HAS_SDL3=ON)")
else()
  if(EE_FETCH_SDL3)
    include(FetchContent)

    # Build a static SDL3 and skip the extras to keep the fetch build lean.
    # These are set before MakeAvailable so SDL3's option() calls honour them.
    set(SDL_SHARED       OFF CACHE BOOL "Build SDL3 shared library"   FORCE)
    set(SDL_STATIC       ON  CACHE BOOL "Build SDL3 static library"   FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build SDL3 test library"     FORCE)
    set(SDL_TESTS        OFF CACHE BOOL "Build SDL3 tests"            FORCE)
    set(SDL_EXAMPLES     OFF CACHE BOOL "Build SDL3 examples"         FORCE)
    set(SDL_INSTALL      OFF CACHE BOOL "Generate SDL3 install rules" FORCE)

    FetchContent_Declare(
      sdl3
      GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
      GIT_TAG        release-3.4.16
      GIT_SHALLOW    TRUE
    )
    message(STATUS "ee: SDL3 not found on system; fetching SDL3 3.4.16 from source (one-time)...")
    FetchContent_MakeAvailable(sdl3)

    if(TARGET SDL3::SDL3)
      set(EE_HAS_SDL3 ON)
      message(STATUS "ee: SDL3 3.4.16 fetched and built from source (EE_HAS_SDL3=ON)")
    else()
      message(WARNING "ee: SDL3 fetch did not produce the SDL3::SDL3 target; "
                      "GUI apps will build as stubs. Reconfigure with "
                      "-DEE_FETCH_SDL3=OFF to silence this, or install SDL3.")
    endif()
  else()
    message(STATUS "ee: SDL3 not found and EE_FETCH_SDL3=OFF; "
                   "GUI apps will build as stubs (EE_HAS_SDL3=OFF)")
  endif()
endif()
