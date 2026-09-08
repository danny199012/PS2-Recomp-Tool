find_package(SDL3 QUIET)
if(SDL3_FOUND OR TARGET SDL3::SDL3)
  set(EE_HAS_SDL3 ON)
  add_executable(ee-studio main.cpp)
  target_link_libraries(ee-studio PRIVATE ee::runtime ee::elf ee::analysis ee::codegen)
  target_compile_definitions(ee-studio PRIVATE EE_HAS_SDL3)
  if(TARGET SDL3::SDL3)
    target_link_libraries(ee-studio PRIVATE SDL3::SDL3)
  else()
    target_include_directories(ee-studio PRIVATE ${SDL3_INCLUDE_DIRS})
    target_link_libraries(ee-studio PRIVATE ${SDL3_LIBRARIES})
  endif()

  # Try to find Dear ImGui via FetchContent (optional).
  option(EE_WITH_IMGUI "Build ee-studio with Dear ImGui" OFF)
  if(EE_WITH_IMGUI)
    include(FetchContent)
    FetchContent_Declare(
      imgui
      GIT_REPOSITORY https://github.com/ocornut/imgui.git
      GIT_TAG v1.91.5
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)
    target_compile_definitions(ee-studio PRIVATE EE_HAS_IMGUI)
    # ImGui SDL3 backends are in imgui/backends/
    target_sources(ee-studio PRIVATE
      ${imgui_SOURCE_DIR}/imgui.cpp
      ${imgui_SOURCE_DIR}/imgui_draw.cpp
      ${imgui_SOURCE_DIR}/imgui_tables.cpp
      ${imgui_SOURCE_DIR}/imgui_widgets.cpp
      ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
      ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    )
    target_include_directories(ee-studio PRIVATE ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  endif()

  message(STATUS "ee-studio: SDL3 found, building GUI launcher (EE_WITH_IMGUI=${EE_WITH_IMGUI})")
else()
  message(STATUS "ee-studio: SDL3 not found, building headless stub")
  add_executable(ee-studio main.cpp)
  target_link_libraries(ee-studio PRIVATE ee::runtime ee::elf ee::analysis ee::codegen)
endif()
