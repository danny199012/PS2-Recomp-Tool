# SPDX-License-Identifier: GPL-3.0-only

# game-launcher: per-game **release** launcher template.
#
# A single game's PC port is one executable built from this template plus the
# game's recompiled code. The launcher ships no game assets: on first run its
# GUI asks the user for a legally obtained PS2 disc image (.iso/.bin), reads
# SYSTEM.CNF, extracts the boot ELF, and runs the embedded recompiled game.
#
# Build a title with, e.g.:
#   cmake -S . -B build -DEE_GAME_NAME="SomeGame" \
#         -DEE_GAME_RECOMP_SOURCES="C:/games/somegame/somegame.recomp.cpp" \
#         -DEE_GAME_OVERRIDES_SOURCES="C:/games/somegame/overrides.cpp" \
#         -DEE_GAME_BOOT_ELF="SLUS_123.45" \
#         -DEE_GAME_EXPECTED_CRC=0x12345678
#
# Without SDL3 (or with EE_LAUNCHER_IMGUI=OFF) it builds as a headless CLI:
#   game-launcher --extract <disc.iso> [--out dir]   extract the boot ELF
#   game-launcher --run <disc.iso>                   run the game (stdout)

# SDL3 is acquired by the top-level cmake/ee_sdl3.cmake module (system SDL3,
# or fetched/built from source when EE_FETCH_SDL3=ON). It sets EE_HAS_SDL3
# and provides the SDL3::SDL3 target. Without SDL3 (or EE_LAUNCHER_IMGUI=OFF)
# the launcher builds as a headless CLI (see modes documented above).
option(EE_LAUNCHER_IMGUI "Build the game-launcher GUI with Dear ImGui (requires SDL3)" ON)

# --- per-game identity (launcher_config.h) -----------------------------------
# These are the per-game knobs discussed in docs/LAUNCHER.md. Pass them with -D
# when building a title's launcher, e.g. -DEE_GAME_NAME="Ratchet & Clank".
set(EE_GAME_NAME "" CACHE STRING "Game name shown in the launcher GUI")
set(EE_GAME_BOOT_ELF "" CACHE STRING "Expected boot ELF leaf name (e.g. SLUS_203.61)")
set(EE_GAME_EXPECTED_CRC 0 CACHE STRING "Expected boot ELF CRC32 (0 = accept any)")
set(EE_GAME_RECOMP_SOURCES "" CACHE STRING "Path(s) to the game's recompiled .cpp files")
set(EE_GAME_OVERRIDES_SOURCES "" CACHE STRING "Path(s) to the game's override .cpp files")

set(EE_GAME_EXPECTED_CRC_U 0)
if(EE_GAME_EXPECTED_CRC)
  math(EXPR EE_GAME_EXPECTED_CRC_U "${EE_GAME_EXPECTED_CRC}")
endif()
set(EE_GAME_HAS_RECOMP 0)
if(EE_GAME_RECOMP_SOURCES)
  set(EE_GAME_HAS_RECOMP 1)
endif()
configure_file(launcher_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/launcher_config.h)

add_executable(game-launcher main.cpp)
target_include_directories(game-launcher PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(game-launcher PRIVATE ee::runtime ee::elf)

if(EE_HAS_SDL3 AND EE_LAUNCHER_IMGUI)
  target_compile_definitions(game-launcher PRIVATE EE_HAS_SDL3 EE_HAS_IMGUI)
  if(TARGET SDL3::SDL3)
    target_link_libraries(game-launcher PRIVATE SDL3::SDL3)
  else()
    target_include_directories(game-launcher PRIVATE ${SDL3_INCLUDE_DIRS})
    target_link_libraries(game-launcher PRIVATE ${SDL3_LIBRARIES})
  endif()

  include(FetchContent)
  FetchContent_Declare(
    launcher_imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(launcher_imgui)
  target_sources(game-launcher PRIVATE
    ${launcher_imgui_SOURCE_DIR}/imgui.cpp
    ${launcher_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${launcher_imgui_SOURCE_DIR}/imgui_tables.cpp
    ${launcher_imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${launcher_imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${launcher_imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  )
  target_include_directories(game-launcher PRIVATE
    ${launcher_imgui_SOURCE_DIR} ${launcher_imgui_SOURCE_DIR}/backends)
  message(STATUS "game-launcher: building GUI (SDL3 + Dear ImGui)")
else()
  message(STATUS "game-launcher: building headless CLI "
                 "(SDL3 unavailable or EE_LAUNCHER_IMGUI=OFF)")
endif()

if(EE_GAME_RECOMP_SOURCES)
  target_sources(game-launcher PRIVATE ${EE_GAME_RECOMP_SOURCES})
endif()
if(EE_GAME_OVERRIDES_SOURCES)
  target_sources(game-launcher PRIVATE ${EE_GAME_OVERRIDES_SOURCES})
endif()