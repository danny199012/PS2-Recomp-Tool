add_library(ee_base INTERFACE)
target_include_directories(ee_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(ee::base ALIAS ee_base)
