add_library(ee_r5900 STATIC src/r5900.cpp)
target_include_directories(ee_r5900 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ee_r5900 PUBLIC ee::base)
add_library(ee::r5900 ALIAS ee_r5900)
