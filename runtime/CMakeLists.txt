add_library(ee_runtime STATIC src/runtime.cpp)
target_include_directories(ee_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ee_runtime PUBLIC ee::base ee::elf)
add_library(ee::runtime ALIAS ee_runtime)
