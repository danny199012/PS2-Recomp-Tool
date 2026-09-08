add_library(ee_analysis STATIC src/analysis.cpp)
target_include_directories(ee_analysis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ee_analysis PUBLIC ee::base ee::elf ee::r5900)
add_library(ee::analysis ALIAS ee_analysis)
