# SPDX-License-Identifier: GPL-3.0-only

# ee-tools: GUI for the ee-disasm / ee-analyze / ee-recomp toolchain.
# One window with tabs:
#   - ELF Info   (ee-disasm header/sections/symbols)
#   - Disassemble (ee-disasm)
#   - Analyze     (ee-analyze: function discovery + TOML/CSV/JSON export)
#   - Recompile   (ee-recomp: emit C++ from the ELF + config)
# The command-line tools remain the scripting-friendly equivalent.
#
# Requires SDL3 + Dear ImGui (fetched automatically). Without SDL3 it builds
# as a stub that prints a message.

find_package(SDL3 QUIET)
if(SDL3_FOUND OR TARGET SDL3::SDL3)
  set(EE_HAS_SDL3 ON)
else()
  set(EE_HAS_SDL3 OFF)
endif()

option(EE_TOOLS_IMGUI "Build the ee-tools GUI with Dear ImGui (requires SDL3)" ON)

add_executable(ee-tools main.cpp)
target_link_libraries(ee-tools PRIVATE ee::elf ee::r5900 ee::analysis ee::codegen)

if(EE_HAS_SDL3 AND EE_TOOLS_IMGUI)
  target_compile_definitions(ee-tools PRIVATE EE_HAS_SDL3 EE_HAS_IMGUI)
  if(TARGET SDL3::SDL3)
    target_link_libraries(ee-tools PRIVATE SDL3::SDL3)
  else()
    target_include_directories(ee-tools PRIVATE ${SDL3_INCLUDE_DIRS})
    target_link_libraries(ee-tools PRIVATE ${SDL3_LIBRARIES})
  endif()

  include(FetchContent)
  FetchContent_Declare(
    tools_imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(tools_imgui)
  target_sources(ee-tools PRIVATE
    ${tools_imgui_SOURCE_DIR}/imgui.cpp
    ${tools_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${tools_imgui_SOURCE_DIR}/imgui_tables.cpp
    ${tools_imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${tools_imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${tools_imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
  )
  target_include_directories(ee-tools PRIVATE
    ${tools_imgui_SOURCE_DIR} ${tools_imgui_SOURCE_DIR}/backends)
endif()