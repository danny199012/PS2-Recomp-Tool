find_package(Threads REQUIRED)

# Core runtime sources (always included)
set(RUNTIME_SOURCES
    src/runtime.cpp
    src/kernel.cpp
    src/hw.cpp
    src/vu.cpp
    src/gs_renderer.cpp
    src/vu1.cpp
    src/cdvd.cpp
    src/iop.cpp
    src/game_overrides.cpp
)

# Game-specific overrides (conditionally compiled via CMake options)
option(RUNTIME_INCLUDE_URBZ_OVERRIDES "Include The Sims: Urban Stories game override" ON)
if(RUNTIME_INCLUDE_URBZ_OVERRIDES)
    list(APPEND RUNTIME_SOURCES src/game_overrides_urbz.cpp)
endif()

add_library(ee_runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(ee_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ee_runtime PUBLIC ee::base ee::elf Threads::Threads)
add_library(ee::runtime ALIAS ee_runtime)
