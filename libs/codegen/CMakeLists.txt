add_library(ee_codegen STATIC src/codegen.cpp)
target_include_directories(ee_codegen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ee_codegen PUBLIC ee::base ee::analysis ee::r5900)
add_library(ee::codegen ALIAS ee_codegen)
