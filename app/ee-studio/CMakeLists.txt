# ee-studio: Game library + pipeline runner GUI for EERecomp.
# Built with SDL3 (acquired once by the top-level cmake/ee_sdl3.cmake module:
# system SDL3, or fetched/built from source when EE_FETCH_SDL3=ON) + Dear ImGui.
# Without SDL3 the app compiles as a headless stub that explains SDL3 is
# required and waits so the window does not flash.
#
# Dear ImGui is ON by default: without it the SDL3 path has no UI to render and
# just opens an empty window (which previously closed after 100 ms, looking like
# "ee-studio doesn't work"). Pass -DEE_WITH_IMGUI=OFF for the raw-SDL3 skeleton.

add_executable(ee-studio main.cpp)
target_link_libraries(ee-studio PRIVATE ee::runtime ee::elf ee::analysis ee::codegen)

if(EE_HAS_SDL3)
  target_compile_definitions(ee-studio PRIVATE EE_HAS_SDL3)
  if(TARGET SDL3::SDL3)
    target_link_libraries(ee-studio PRIVATE SDL3::SDL3)
  else()
    target_include_directories(ee-studio PRIVATE ${SDL3_INCLUDE_DIRS})
    target_link_libraries(ee-studio PRIVATE ${SDL3_LIBRARIES})
  endif()

  # Dear ImGui is on by default (see header comment); raw SDL3 skeleton is the
  # opt-out via -DEE_WITH_IMGUI=OFF.
  option(EE_WITH_IMGUI "Build ee-studio with Dear ImGui" ON)
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

  message(STATUS "ee-studio: building GUI (EE_WITH_IMGUI=${EE_WITH_IMGUI})")
else()
  message(STATUS "ee-studio: SDL3 unavailable (EE_HAS_SDL3=OFF); building headless stub")
endif()
