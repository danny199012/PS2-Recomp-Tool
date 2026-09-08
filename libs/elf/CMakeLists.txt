add_library(ee_elf STATIC src/elf.cpp)
target_include_directories(ee_elf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ee_elf PUBLIC ee::base)
add_library(ee::elf ALIAS ee_elf)
